/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

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

#include "vkpt.h"
#include "DLSS.h"
#include "reflex.h"
#include "system/system.h"
#include "common/cpuprof.h"	// cpuprof_wait_us: the CPU's own frame time

#include <assert.h>
#include <time.h>


extern cvar_t *cvar_profiler;
extern cvar_t *cvar_profiler_scale;
extern cvar_t *cvar_pt_reflect_refract;
extern cvar_t *cvar_pt_fog_froxel;
extern cvar_t *cvar_pt_sharc;
extern cvar_t *cvar_pt_restir_gi;
extern cvar_t *cvar_profiler_samples;

// Sample data for a single profiled value
typedef struct profiler_entry_samples_s
{
	// Circular buffer with collected samples
	uint64_t *data;
	// Number of collected samples
	size_t num_samples;
	// Index where next sample should be written
	size_t next_idx;
	// Sum of all sample values
	uint64_t accumulated;
} profiler_entry_samples_t;

static struct {
	VkQueryPool query_pool;

	uint64_t query_pool_results[NUM_PROFILER_QUERIES_PER_FRAME];

	bool queries_used[NUM_PROFILER_QUERIES_PER_FRAME * MAX_FRAMES_IN_FLIGHT];

	// Number of sample circular buffer size (same for each entry)
	size_t allocated_samples;
	// Sample data for profiled values
	profiler_entry_samples_t samples[NUM_PROFILER_ENTRIES];

	// the results are the D3D12 renderer's (vkpt_profiler_take_results)
	bool d3d12;
} profiler_data;

/* UPLOAD SPIKE TRACKER - diagnostic, read out by profiler_dump.

   The overlay reported "upload lights" at 43 ms average against a 13.8 ms frame.
   That marker brackets two tiny copies on the TRANSFER queue, so its span is
   wall-clock time on that queue: whatever it spends stalled counts, and none of
   it is inside FRAME_TIME, which starts in the trace command buffer after the
   transfer semaphore has been waited on. A 60-sample average cannot say whether
   that is one huge stall or a steady cost, nor whether it lines up with the
   frame drops the player sees. This counts it per frame, next to the CPU frame
   interval, so the two can be compared. The GPU values are read back
   MAX_FRAMES_IN_FLIGHT frames late, so the CPU side uses the worst interval
   over the last MAX_FRAMES_IN_FLIGHT + 1 frames. */
#define SPIKE_WORST 8
typedef struct {
	float upload_ms, gpu_frame_ms, cpu_ms;
} upload_spike_t;

static struct {
	uint64_t last_us;
	float cpu_recent[MAX_FRAMES_IN_FLIGHT + 1];
	int cpu_recent_idx;

	int frames;
	double upload_sum;
	float upload_max, cpu_max;
	int upload_over_2, upload_over_5, upload_over_20;
	int cpu_over_25, cpu_over_50;
	int both; // upload > 5 ms AND a CPU interval > 25 ms in the window
	upload_spike_t worst[SPIKE_WORST];
} upload_spikes;

static void
upload_spikes_reset(void)
{
	uint64_t last_us = upload_spikes.last_us;
	memset(&upload_spikes, 0, sizeof(upload_spikes));
	upload_spikes.last_us = last_us;
}

static void
upload_spikes_record(float upload_ms, float gpu_frame_ms)
{
	uint64_t now = Sys_Microseconds();
	float cpu_ms = upload_spikes.last_us ? (float)(now - upload_spikes.last_us) * 1e-3f : 0.f;
	upload_spikes.last_us = now;

	upload_spikes.cpu_recent[upload_spikes.cpu_recent_idx] = cpu_ms;
	upload_spikes.cpu_recent_idx = (upload_spikes.cpu_recent_idx + 1) % LENGTH(upload_spikes.cpu_recent);
	float cpu_window = 0.f;
	for (int i = 0; i < LENGTH(upload_spikes.cpu_recent); i++)
		cpu_window = max(cpu_window, upload_spikes.cpu_recent[i]);

	upload_spikes.frames++;
	upload_spikes.upload_sum += upload_ms;
	upload_spikes.upload_max = max(upload_spikes.upload_max, upload_ms);
	upload_spikes.cpu_max = max(upload_spikes.cpu_max, cpu_ms);
	upload_spikes.upload_over_2 += upload_ms > 2.f;
	upload_spikes.upload_over_5 += upload_ms > 5.f;
	upload_spikes.upload_over_20 += upload_ms > 20.f;
	upload_spikes.cpu_over_25 += cpu_ms > 25.f;
	upload_spikes.cpu_over_50 += cpu_ms > 50.f;
	upload_spikes.both += upload_ms > 5.f && cpu_window > 25.f;

	// keep the SPIKE_WORST largest uploads, sorted descending
	int slot = SPIKE_WORST;
	while (slot > 0 && upload_spikes.worst[slot - 1].upload_ms < upload_ms)
		slot--;
	if (slot < SPIKE_WORST) {
		memmove(&upload_spikes.worst[slot + 1], &upload_spikes.worst[slot],
			(SPIKE_WORST - slot - 1) * sizeof(upload_spike_t));
		upload_spikes.worst[slot] = (upload_spike_t){ upload_ms, gpu_frame_ms, cpu_window };
	}
}

/* PROFILER 2: THE FRAME AS GRAPHS (Matt, 11-07) - frame time and fps over the last
   several seconds, newest on the right (draw_graphs). A sample per frame the
   renderer begins: the time since the one before by the CPU clock, which is what
   the fps counter counts - rendered frames, frame generation's not among them -
   and the whole GPU frame, the table's first row, which arrives
   MAX_FRAMES_IN_FLIGHT frames late. Recorded always, as the GPU times are, so the
   graphs have their history the moment they are turned on. With frame generation,
   the presents per rendered frame too: the fps panel's second line. */
#define GRAPH_SAMPLES 2048	// one a pixel column: the panels are 1408 pixels wide at 4K

/* THE GPU FRAME BY CATEGORY (profiler 3 and 4, Matt 11-14). Each pass's row goes
   in one of these (category_of below). The last three are what used to be one
   "other": the copies and the HUD, the GPU idle waiting for the CPU's list (both
   timed on D3D12 only) and the remainder no row covers. */
enum {
	CAT_FRAME_SETUP,
	CAT_PRIMARY_RAYS,
	CAT_FLOW,
	CAT_VOLUMETRICS,
	CAT_DIRECT,
	CAT_INDIRECT,
	CAT_REFLECT_REFRACT,
	CAT_DENOISER,
	CAT_COMPOSITING,
	CAT_DLSS,
	CAT_POST,
	CAT_OVERHEAD,
	CAT_IDLE,
	CAT_OTHER,
	NUM_CATS
};

/* THE CPU FRAME BY SECTION (profiler 3 and 4, Matt 11-14: "CPU frame time is causing
   the problem ... can we thread this or optimize it"). The client's sections and the
   finer pieces inside them (cpuprof.h), of the last frame CL_CpuStatsFrame closed,
   as a tree: depth 0 adds up to the CPU's work, each deeper row is part of the one
   above it. rbegin and rend are shown without the renderer's waits in them, which
   are counted under "waiting" with the limiter's and Reflex's. */
enum {
	CPUROW_BEGIN,
	CPUROW_VIEW,
	CPUROW_ENTITIES,
	CPUROW_PARTICLES,
	CPUROW_BLOOD_SIM,
	CPUROW_BLOOD_BODIES,
	CPUROW_BLOOD_FILM,
	CPUROW_RENDER,
	CPUROW_SCENE,
	CPUROW_FLOW,
	CPUROW_TRANSPARENT,
	CPUROW_BLOOD_MESH,
	CPUROW_FILM_MESH,
	CPUROW_BLOOD_COPY,
	CPUROW_BLAS,
	CPUROW_RECORD,
	CPUROW_RENDER_REST,
	CPUROW_HUD,
	CPUROW_END,
	CPUROW_SERVER,
	CPUROW_CLIENT,
	CPUROW_SOUND,
	CPUROW_OTHER,
	CPUROW_TOTAL,
	CPUROW_WAITING,
	/* WHICH WAIT (11-14): Matt's dip captures held frames of 18-20 ms frame to frame
	   whose own CPU work (4-11 ms) and GPU work (13-16 ms) were both under 16.67 -
	   the rest was "waiting", and that alone did not say on what. The waits it adds
	   up, each where it is timed (cpuprof.h). */
	CPUROW_WAIT_LIMITER,
	CPUROW_WAIT_REFLEX,
	CPUROW_WAIT_SWAPCHAIN,
	CPUROW_WAIT_FENCE,
	CPUROW_WAIT_PRESENT,
	NUM_CPUROWS
};

static const struct {
	const char *name;
	int depth;		// -1: a summary row
} cpurow_info[NUM_CPUROWS] = {
	[CPUROW_BEGIN]        = { "begin frame", 0 },
	[CPUROW_VIEW]         = { "view setup", 0 },
	[CPUROW_ENTITIES]     = { "entities", 1 },
	[CPUROW_PARTICLES]    = { "particles + blood", 1 },
	[CPUROW_BLOOD_SIM]    = { "blood sim", 2 },
	[CPUROW_BLOOD_BODIES] = { "vs monster meshes", 3 },
	[CPUROW_BLOOD_FILM]   = { "blood film", 2 },
	[CPUROW_RENDER]       = { "render", 0 },
	[CPUROW_SCENE]        = { "scene setup", 1 },
	[CPUROW_FLOW]         = { "nvidia flow", 1 },
	[CPUROW_TRANSPARENT]  = { "particles to geometry", 1 },
	[CPUROW_BLOOD_MESH]   = { "blood meshes", 1 },
	[CPUROW_FILM_MESH]    = { "film tiles", 2 },
	[CPUROW_BLOOD_COPY]   = { "blood upload copy", 1 },
	[CPUROW_BLAS]         = { "blas plan, instances", 1 },
	[CPUROW_RECORD]       = { "command list", 1 },
	[CPUROW_RENDER_REST]  = { "rest of render", 1 },
	[CPUROW_END]          = { "end frame", 0 },
	[CPUROW_SERVER]       = { "server", 0 },
	[CPUROW_CLIENT]       = { "net, demo, predict", 0 },
	[CPUROW_SOUND]        = { "sound", 0 },
	[CPUROW_OTHER]        = { "other", 0 },
	[CPUROW_HUD]          = { "hud, menus, console", 0 },
	[CPUROW_TOTAL]        = { "cpu work (sum)", -1 },
	[CPUROW_WAITING]      = { "waiting (not work)", -1 },
	[CPUROW_WAIT_LIMITER] = { "fps limiter sleep", 1 },	// Qcommon_Frame's NET_Sleep and spin
	[CPUROW_WAIT_REFLEX]  = { "reflex sleep", 1 },
	[CPUROW_WAIT_SWAPCHAIN] = { "swapchain wait", 1 },	// D3D12's waitable, Vulkan's acquire
	[CPUROW_WAIT_FENCE]   = { "gpu fence wait", 1 },	// an older frame's GPU work, its slot reused
	[CPUROW_WAIT_PRESENT] = { "present call", 1 },
};

static void
cpu_rows_of(const cpuprof_frame_t *f, float *row, uint32_t *count)
{
#define SECT(id)	((float)f->us[CPUPROF_##id] * 1e-3f)
#define DET(id)		((float)f->detail_us[CPUPROF_D_##id] * 1e-3f)
	row[CPUROW_BEGIN]        = max(SECT(R_BEGIN) - DET(WAIT_BEGIN), 0.f);
	row[CPUROW_VIEW]         = SECT(VIEW_PREP);
	row[CPUROW_ENTITIES]     = DET(ENTITIES);
	row[CPUROW_PARTICLES]    = DET(PARTICLES);
	row[CPUROW_BLOOD_SIM]    = DET(BLOOD_SIM);
	row[CPUROW_BLOOD_BODIES] = DET(BLOOD_BODIES);
	row[CPUROW_BLOOD_FILM]   = DET(BLOOD_FILM);
	row[CPUROW_RENDER]       = SECT(R_RENDER);
	row[CPUROW_SCENE]        = DET(SCENE);
	row[CPUROW_FLOW]         = DET(FLOW);
	row[CPUROW_TRANSPARENT]  = DET(TRANSPARENT);
	row[CPUROW_BLOOD_MESH]   = DET(BLOOD_MESH);
	row[CPUROW_FILM_MESH]    = DET(FILM_MESH);
	row[CPUROW_BLOOD_COPY]   = DET(BLOOD_COPY);
	row[CPUROW_BLAS]         = DET(BLAS);
	// D3D12's command list has the blood copy in it; Vulkan's is not timed
	row[CPUROW_RECORD]       = DET(RECORD) > 0.f ? max(DET(RECORD) - DET(BLOOD_COPY), 0.f) : 0.f;
	float parts = 0.f;
	for (int r = CPUROW_SCENE; r < CPUROW_RENDER_REST; r++)
		if (cpurow_info[r].depth == 1)
			parts += row[r];
	row[CPUROW_RENDER_REST]  = max(row[CPUROW_RENDER] - parts, 0.f);
	row[CPUROW_HUD]          = SECT(HUD);
	row[CPUROW_END]          = max(SECT(R_END) - DET(WAIT_PRESENT), 0.f);
	row[CPUROW_SERVER]       = SECT(SERVER);
	row[CPUROW_CLIENT]       = SECT(CL_EVENTS) + SECT(DEMO) + SECT(PREDICT);
	row[CPUROW_SOUND]        = SECT(SOUND);
	row[CPUROW_OTHER]        = (float)f->us[CPUPROF_COUNT] * 1e-3f;
	float total = 0.f;
	for (int r = 0; r < CPUROW_TOTAL; r++)
		if (cpurow_info[r].depth == 0)
			total += row[r];
	row[CPUROW_TOTAL]        = total;
	row[CPUROW_WAITING]      = SECT(NET_SLEEP) + SECT(REFLEX) + DET(WAIT_BEGIN) + DET(WAIT_PRESENT);
	row[CPUROW_WAIT_LIMITER] = SECT(NET_SLEEP);
	row[CPUROW_WAIT_REFLEX]  = SECT(REFLEX);
	row[CPUROW_WAIT_SWAPCHAIN] = max(DET(WAIT_BEGIN) - DET(WAIT_FENCE), 0.f);
	row[CPUROW_WAIT_FENCE]   = DET(WAIT_FENCE);
	row[CPUROW_WAIT_PRESENT] = DET(WAIT_PRESENT);
#undef SECT
#undef DET
	memcpy(count, f->count, sizeof(f->count));
}

// "blood: ..." - what the blood work was, beside its times
static void
blood_count_text(char *line1, char *line2, size_t size, const uint32_t *c)
{
	Q_snprintf(line1, size, "blood: %u in the air, %u landed; %u traces, %u on monster meshes",
		c[CPUPROF_C_BLOOD_AIR], c[CPUPROF_C_BLOOD_LANDED], c[CPUPROF_C_BLOOD_TRACES],
		c[CPUPROF_C_BLOOD_BODY_TRACES]);
	Q_snprintf(line2, size, "blood meshes: %u moved (%u on the gpu), %u rebuilt, %u still; up %.1f MB in %u regions",
		c[CPUPROF_C_BLOOD_MOVED], c[CPUPROF_C_BLOOD_GPU_MOVES], c[CPUPROF_C_BLOOD_REBUILT],
		c[CPUPROF_C_BLOOD_STILL], (float)c[CPUPROF_C_BLOOD_UP_KB] / 1024.f, c[CPUPROF_C_BLOOD_COPIES]);
}

static struct {
	uint64_t last_us;
	uint64_t last_wait_us;	// cpuprof_wait_us at the last frame
	uint32_t last_serial;	// the CPU frame the last sample was (0 = not known)
	float frame_ms[GRAPH_SAMPLES];
	/* the CPU's own time in that frame: frame_ms less what the main thread spent
	   waiting (cpuprof.h - the limiter's sleep, Reflex's, the fence, the swapchain,
	   the present). GPU-bound, frame_ms is the GPU's pace and this is the CPU's work;
	   Matt 11-14: "the cpu frame to frame mostly just matches the gpu number". */
	float cpu_ms[GRAPH_SAMPLES];
	float gpu_ms[GRAPH_SAMPLES];	// 0 where the GPU frame was not timed
	uint8_t presents[GRAPH_SAMPLES];	// DLSS-G's presents per rendered frame, 1 without
	float cat_ms[GRAPH_SAMPLES][NUM_CATS];	// the GPU frame by category (categories_of)
	float gap_ms[GRAPH_SAMPLES];	// the GPU idle before the frame, in no frame (D3D12; profiler_frame_extras_t)
	/* THE CAPTURE'S OWN FRAMES (Matt 11-15: "the screen shot doesn't seem to capture
	   the actual cpu spikes" - it cannot: the spike WAS the screenshot, 250 ms of
	   readback then, ~40 now). The CPU frames CAPTURE_SKIP covers: profiler 3 draws
	   them as a faint column with no CPU line, and no key's average or max counts
	   them - the dip test already left them out. */
	uint8_t shot[GRAPH_SAMPLES];
	float cpu_rows[GRAPH_SAMPLES][NUM_CPUROWS];	// the CPU frame by section (cpu_rows_of)
	uint32_t counts[GRAPH_SAMPLES][CPUPROF_COUNTER_COUNT];
	int next, count;
	// the panels' tops (graph_scale); cat_top is profiler 3's and 4's
	float frame_top, fps_top, cat_top;
	// the numbers printed, refreshed twice a second so they can be read
	unsigned text_ms;
	char text[5][64];
} graph;

/* THE PANELS' TOPS (Matt, 11-07). Frame time: 33.3 ms (30 fps), so its middle
   line is 16.7 ms (60 fps). Fps: 120, its middle line 60. Each doubled for what
   does not fit, the middle line then being the top before, and changed only on a
   second of evidence, both ways - it followed every frame in view at first, and a
   level load's spikes held the frame time's at seconds long after:
   - up once the panel's values have kept coming in over the top for a second, to
     fit the largest of them. Counted from the first one's END, so one long frame
     (a load's) is never a second of them, however long it took;
   - down to the smallest top every value of the last second fits, which is the
     default again a second after a spike.
   A panel's value at a frame is the largest of its lines there: the frame's time
   or the GPU's; the rendered fps or frame generation's presented. */
#define GRAPH_FRAME_TOP (1000.f / 30.f)
#define GRAPH_FPS_TOP   120.f

static float
graph_frame_value(int k)
{
	return max(graph.frame_ms[k], graph.gpu_ms[k]);
}

static float
graph_fps_value(int k)
{
	return 1000.f / graph.frame_ms[k] * (float)max(graph.presents[k], 1);
}

static float
graph_fit(float base, float v)
{
	float top = base;
	while (top < v && top < base * 1024.f)
		top *= 2.f;
	return top;
}

static void
graph_scale(float *top, float base, float (*value)(int k))
{
	if (*top < base)
		*top = base;

	// Up: back from the newest frame through the run over the top. Reaching one
	// more frame of the run with a second already behind it means the run has
	// lasted a second since that frame ended; the top then fits that second's
	// values, not the one that started it. A frame of a second or more is a stall
	// (a load's), not a frame rate: it ends the run - else a slow frame before one
	// made a second of "evidence" with it and the top went to 2 s (the scale test,
	// 11-07).
	float run = 0.f, run_largest = 0.f;
	for (int i = 0; i < graph.count; i++)
	{
		const int k = (graph.next - 1 - i + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		const float v = value(k);
		if (!(v > *top) || graph.frame_ms[k] >= 1000.f)
			break;
		if (run >= 1000.f)
		{
			*top = graph_fit(base, run_largest);
			return;
		}
		run += graph.frame_ms[k];
		run_largest = max(run_largest, v);
	}

	// Down: the largest value of the last second, once there is a second of them
	float span = 0.f, largest = 0.f;
	for (int i = 0; i < graph.count && span < 1000.f; i++)
	{
		const int k = (graph.next - 1 - i + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		span += graph.frame_ms[k];
		largest = max(largest, value(k));
	}
	if (span >= 1000.f && graph_fit(base, largest) < *top)
		*top = graph_fit(base, largest);
}

/* THE CATEGORIES (Matt, 11-14). ReSTIR DI has no row of its own: it runs inside
   the direct lighting pass, so it is in that one already. The Flow march is only
   its own row under pt_flow_split_pass 1; otherwise it is inside primary rays.
   ASVGF's TAA is with the denoiser it belongs to (an empty span under DLSS). */
/* THE COLOURS: as shown in the game (Matt 11-14, after a scare - the HDR screenshots
   made them look alike; on screen they are fine). The three greys are what is not a
   pass, darker to lighter up the stack, the idle GPU's striped as well (draw_stack). */
static const struct {
	const char *name;
	uint8_t r, g, b;
} cat_info[NUM_CATS] = {
	[CAT_FRAME_SETUP]     = { "frame setup",          0, 215, 255 },
	[CAT_PRIMARY_RAYS]    = { "primary rays",        50,  80, 235 },
	[CAT_FLOW]            = { "nvidia flow",        255, 140,   0 },
	[CAT_VOLUMETRICS]     = { "volumetrics",        165,  70, 210 },
	[CAT_DIRECT]          = { "direct lighting",    255, 230,  40 },
	[CAT_INDIRECT]        = { "indirect lighting",   40, 200,  90 },
	[CAT_REFLECT_REFRACT] = { "reflect refract",    255,  60, 200 },
	[CAT_DENOISER]        = { "denoiser",           170, 120,  70 },
	[CAT_COMPOSITING]     = { "compositing",        255, 170, 190 },
	[CAT_DLSS]            = { "dlss",               225,  40,  40 },
	[CAT_POST]            = { "post processing",    210, 210, 255 },
	[CAT_OVERHEAD]        = { "uploads, hud, copy",  95,  95,  95 },
	[CAT_IDLE]            = { "gpu idle: cpu late", 150, 150, 150 },
	[CAT_OTHER]           = { "unaccounted",        235, 235, 235 },
};

static uint32_t
display_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	return MakeColor(r, g, b, a);
}

static uint32_t
cat_color(int c)
{
	return display_color(cat_info[c].r, cat_info[c].g, cat_info[c].b, 255);
}

static int
category_of(int idx)
{
	switch (idx)
	{
	case PROFILER_UPLOAD_LIGHTS:
	case PROFILER_SHADOW_MAP:
	case PROFILER_INSTANCE_GEOMETRY:
	case PROFILER_BVH_UPDATE:
	case PROFILER_UPDATE_ENVIRONMENT:
		return CAT_FRAME_SETUP;
	case PROFILER_PRIMARY_RAYS:
		return CAT_PRIMARY_RAYS;
	case PROFILER_FLOW_SIMULATION:
	case PROFILER_FLOW_SMOKE_LIGHTING:
	case PROFILER_FLOW_MARCH:
		return CAT_FLOW;
	case PROFILER_GOD_RAYS:
	case PROFILER_GOD_RAYS_REFLECT_REFRACT:
	case PROFILER_GOD_RAYS_FILTER:
	case PROFILER_FOG_FROXEL:
	case PROFILER_FOG_FROXEL_RESERVOIR:
	case PROFILER_FOG_FROXEL_SPATIAL:
	case PROFILER_FOG_FROXEL_SCATTER:
	case PROFILER_FOG_FROXEL_INTEGRATE:
		return CAT_VOLUMETRICS;
	case PROFILER_DIRECT_LIGHTING:
		return CAT_DIRECT;
	case PROFILER_INDIRECT_LIGHTING:
	case PROFILER_INDIRECT_LIGHTING_0:
	case PROFILER_INDIRECT_LIGHTING_1:
	case PROFILER_SHARC:
	case PROFILER_RESTIR_GI:
		return CAT_INDIRECT;
	case PROFILER_REFLECT_REFRACT_1:
	case PROFILER_REFLECT_REFRACT_2:
		return CAT_REFLECT_REFRACT;
	case PROFILER_ASVGF_GRADIENT_REPROJECT:
	case PROFILER_ASVGF_FULL:
	case PROFILER_ASVGF_RECONSTRUCT_GRADIENT:
	case PROFILER_ASVGF_TEMPORAL:
	case PROFILER_ASVGF_ATROUS:
	case PROFILER_ASVGF_TAA:
		return CAT_DENOISER;
	case PROFILER_COMPOSITING:
	case PROFILER_ACCUMULATION:		// photo mode's running average (profiler.h)
	case PROFILER_INTERLEAVE:
	case PROFILER_MGPU_TRANSFERS:
		return CAT_COMPOSITING;
	case PROFILER_DLSS:
	case PROFILER_DLSS5:
		return CAT_DLSS;
	case PROFILER_MOTION_BLUR:
	case PROFILER_CHROMATIC_ABERRATION:
	case PROFILER_BLOOM:
	case PROFILER_TONE_MAPPING:
		return CAT_POST;
	case PROFILER_TEXTURE_UPLOAD:
	case PROFILER_BLOOD_UPLOAD:
	case PROFILER_TRANSPARENT_UPLOAD:
	case PROFILER_UPLOAD_QUEUE:
	case PROFILER_HUD_PRESENT:
		return CAT_OVERHEAD;
	case PROFILER_GPU_IDLE:
		return CAT_IDLE;
	default:
		// FRAME_TIME and GPU_FRAME: the whole frame, not a pass; SOUND_TRACE: on a
		// queue of its own, beside the frame and not in it
		return -1;
	}
}

/* The rows timed INSIDE another row, on both renderers - counted once, as the
   inner row, with the outer row keeping only its own remainder. ASVGF_FULL is
   marked even with the denoiser off, around compositing (so under DLSS-RR it is
   compositing and nothing else). */
static int
parent_of(int idx)
{
	switch (idx)
	{
	case PROFILER_INDIRECT_LIGHTING_0:
	case PROFILER_INDIRECT_LIGHTING_1:
		return PROFILER_INDIRECT_LIGHTING;
	case PROFILER_FOG_FROXEL_RESERVOIR:
	case PROFILER_FOG_FROXEL_SPATIAL:
	case PROFILER_FOG_FROXEL_SCATTER:
	case PROFILER_FOG_FROXEL_INTEGRATE:
		return PROFILER_FOG_FROXEL;
	case PROFILER_ASVGF_RECONSTRUCT_GRADIENT:
	case PROFILER_ASVGF_TEMPORAL:
	case PROFILER_ASVGF_ATROUS:
	case PROFILER_COMPOSITING:
		return PROFILER_ASVGF_FULL;
	default:
		return -1;
	}
}

static double
result_ms(const uint64_t *r, int idx)
{
	const uint64_t begin = r[idx * 2 + 0], end = r[idx * 2 + 1];
	if (begin == 0 || end == 0 || end < begin)
		return 0.0;
	return (double)(end - begin) * 1e-6 * qvk.timestampPeriod;
}

/* One frame's results by category, in ms. Each row counts its own time only (its
   duration less the rows inside it), so nothing is counted twice; OTHER is what is
   left of the whole GPU frame. On D3D12 the copies, the HUD and the idle GPU are
   rows of their own (OVERHEAD, IDLE), so OTHER is only what no row covers in the
   frame's list (barriers and the like - gpu_timers.c's gap line says where). On
   Vulkan they are not timed and are all in OTHER, with the GPU waiting on the CPU
   mid-frame; its froxel fog can run async and the upload is on the transfer queue,
   so the sum can go past the GPU frame - OTHER is then 0. */
static void
categories_of(const uint64_t *r, float *out)
{
	double self[NUM_PROFILER_ENTRIES];
	for (int i = 0; i < NUM_PROFILER_ENTRIES; i++)
		self[i] = result_ms(r, i);
	for (int i = 0; i < NUM_PROFILER_ENTRIES; i++)
	{
		const int p = parent_of(i);
		if (p >= 0 && self[p] > 0.0)
			self[p] -= result_ms(r, i);
	}

	double sum = 0.0;
	for (int c = 0; c < NUM_CATS; c++)
		out[c] = 0.f;
	for (int i = 0; i < NUM_PROFILER_ENTRIES; i++)
	{
		const int c = category_of(i);
		if (c >= 0 && self[i] > 0.0)
		{
			out[c] += (float)self[i];
			sum += self[i];
		}
	}

	const double gpu = result_ms(r, PROFILER_GPU_FRAME);
	out[CAT_OTHER] = gpu > sum ? (float)(gpu - sum) : 0.f;
}

static float
cat_total(const float *v)
{
	float t = 0.f;
	for (int c = 0; c < NUM_CATS; c++)
		t += v[c];
	return t;
}

// profiler 3's panel value at a frame: the stack, or the CPU's work over it
static float
graph_cat_value(int k)
{
	return graph.shot[k] ? cat_total(graph.cat_ms[k]) : max(cat_total(graph.cat_ms[k]), graph.cpu_ms[k]);
}

/* PROFILER CAPTURE (profiler_capture 1, Matt 11-14): a screenshot of the profiler
   when the frame rate drops under the display's refresh rate (profiler_capture_fps
   to pick another) - twice a dip, not once a frame:

   1. the dip's FIRST slow frame, the moment it crosses;
   2. its WORST frame. From the first slow frame the overlay holds the slowest
      frame's numbers instead of the live ones, and is captured once the frame rate
      has been back over the line for CAPTURE_REARM_MS (or the dip has lasted
      CAPTURE_MAX_HOLD_MS). Skipped when the first frame was the worst.

   Then it re-arms for the next dip. A frame is slow when its time - the CPU's frame
   to frame or the GPU's whole frame, whichever is longer, per presented frame under
   frame generation - is over 1000 / fps ms. Each capture also prints its numbers as
   PROFCAPTURE lines (console and log), every row and category.

   The screenshot is taken by the screenshot command, which runs at the top of the
   next frame and reads the frame just shown - the one whose overlay has the
   capture's banner on it. Its own hitch lands in that next frame's CPU time, so
   for CAPTURE_SKIP frames after a capture only the GPU's time counts: otherwise a
   capture would make the dip's worst frame, or start a dip of its own. */
#define CAPTURE_REARM_MS     500.f
#define CAPTURE_MAX_HOLD_MS  5000.f
#define CAPTURE_SKIP         3
/* Samples after a map load that are never a dip (Matt 11-14: of a demo run's eight
   captures, one was rbase1's load itself, 1.5 s, and one the frame base2's started
   in). The GPU's results come in frames late, so the load's own frames land in
   the first few samples after it.
   THE WHOLE LOAD, AND A SECOND AFTER IT (Matt 11-15: "the profiler capture doesn't
   need to capture a spike caused by level load"). The quiet used to start at
   R_BeginRegistration and count samples - and the loading plaque's own frames used
   them up before the map had drawn anything, so every map of the demo loop still
   caught its first world frames: 77-210 ms GPU frames, 61 ms of it the GPU idle
   while the CPU finished the load (the D3D12 garbage pass, the first models).
   Now it holds while Com_LoadActive (through the first world frame, the "Map load:"
   line) and for CAPTURE_LOAD_QUIET samples and CAPTURE_LOAD_QUIET_MS after. */
#define CAPTURE_LOAD_QUIET     8
#define CAPTURE_LOAD_QUIET_MS  1000

static bool level_loaded;

void
vkpt_profiler_level_loaded(void)
{
	level_loaded = true;
}

// the D3D12 renderer's figures for the frame whose results are the latest (gpu_timers.c)
static profiler_frame_extras_t frame_extras = { .gap_after = -1, .gap_before = -1 };

void
vkpt_profiler_take_extras(const profiler_frame_extras_t *x)
{
	frame_extras = *x;
}

static struct {
	cvar_t *on, *fps;
	bool in_dip;		// from a dip's first slow frame until it re-arms
	bool hold;			// the overlay shows held[] (the dip's worst so far), not the live results
	bool release;		// ... through this frame, then live again
	bool worst_done;	// the dip's worst frame was captured
	int skip;			// frames left whose CPU time is a capture's own hitch (frames not known)
	int quiet;			// samples left after a map load (CAPTURE_LOAD_QUIET)
	unsigned quiet_until;	// ... and the time it lasts to (CAPTURE_LOAD_QUIET_MS)
	uint32_t skip_from, skip_to;	// ... the CPU frames that are, where they are known
	float good_ms;		// time back over the line
	float dip_ms;		// time since the dip's first slow frame
	int dip_frames;		// slow frames in it
	float first_value, worst_value, worst_frame, worst_cpu, worst_gpu, threshold_ms;
	uint64_t held[NUM_PROFILER_QUERIES_PER_FRAME];
	float held_cat[NUM_CATS];
	float held_cpu_rows[NUM_CPUROWS];
	uint32_t held_counts[CPUPROF_COUNTER_COUNT];
	profiler_frame_extras_t held_extras;
	int count;			// captures taken, for the file names
	bool banner_now;	// this frame's overlay is the one being captured
	char banner[192];
	char status[192];	// while holding
} cap;

// what the overlay shows: the held frame during a dip, else the latest results
static const uint64_t *
shown_results(void)
{
	return cap.hold ? cap.held : profiler_data.query_pool_results;
}

static const profiler_frame_extras_t *
shown_extras(void)
{
	return cap.hold ? &cap.held_extras : &frame_extras;
}

static const char *
entry_name(int idx)
{
	static const char *const names[] = {
#define PROFILER_DO(name, indent) #name,
		PROFILER_LIST
#undef PROFILER_DO
	};
	return idx >= 0 && idx < NUM_PROFILER_ENTRIES ? names[idx] + 9 : NULL;
}

// the gap line: "longest 6.20 ms, after BVH_UPDATE, before SHADOW_MAP"
static void
gap_text(char *buf, size_t size, const profiler_frame_extras_t *x)
{
	const char *after = entry_name(x->gap_after), *before = entry_name(x->gap_before);
	Q_snprintf(buf, size, "longest %.2f ms, after %s, before %s", x->gap_largest_ms,
		after ? after : "the list's start", before ? before : "the list's end");
}

static float
capture_threshold_fps(void)
{
	float fps = cap.fps ? cap.fps->value : 0.f;
	if (fps <= 0.f)
	{
		const int hz = Reflex_DisplayRefreshHz();
		fps = hz > 0 ? (float)hz : 60.f;
	}
	return fps;
}

// the latest results become the held frame
static void
capture_hold_latest(float value, float frame_ms, float cpu_ms, float gpu_ms)
{
	memcpy(cap.held, profiler_data.query_pool_results, sizeof(cap.held));
	const int latest = (graph.next - 1 + GRAPH_SAMPLES) % GRAPH_SAMPLES;
	memcpy(cap.held_cat, graph.cat_ms[latest], sizeof(cap.held_cat));
	memcpy(cap.held_cpu_rows, graph.cpu_rows[latest], sizeof(cap.held_cpu_rows));
	memcpy(cap.held_counts, graph.counts[latest], sizeof(cap.held_counts));
	cap.held_extras = frame_extras;
	cap.worst_value = value;
	cap.worst_frame = frame_ms;
	cap.worst_cpu = cpu_ms;
	cap.worst_gpu = gpu_ms;
	cap.hold = true;
}

static void
capture_take(const char *which, int nth)
{
	char stamp[32] = "";
	const time_t now = time(NULL);
	const struct tm *lt = localtime(&now);
	if (lt)
		strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", lt);

	char name[MAX_QPATH];
	Q_snprintf(name, sizeof(name), "profcap/%s_%03d_%s", stamp, ++cap.count % 1000, which);
	/* SDR PNG, HDR display or not (Matt 11-15: in the .hdr shots nvidia flow's
	   orange and direct lighting's yellow looked the same; in the game they are
	   fine). The UI is drawn at ui_hdr_nits - Matt's 500, 6.25x scRGB's 1.0 - so
	   the .hdr file holds orange as (6.25, 1.64, 0) and yellow as (6.25, 4.94,
	   0.13); a viewer clipping each channel at 1.0 shows both as (1, 1, 0), the
	   same pure yellow. D3D12's PNG
	   readback (d3d12_read_frame_target) divides by that white, so the overlay
	   comes out as drawn - the scene clips above it, which a review of the
	   numbers does not need. Vulkan's PNG readback takes only an 8-bit swapchain,
	   so an HDR one there stays .hdr. */
	const bool hdr = R_IsHDR && R_IsHDR() && !profiler_data.d3d12;
	Cbuf_AddText(&cmd_buffer, va("%s %s\n", hdr ? "screenshothdr" : "screenshotpng", name));
	cap.skip = CAPTURE_SKIP;
	// The command runs at the top of the frame after the one being made, which
	// closes as cpuprof_last.serial + 2: a sample's CPU time is the capture's own
	// from the frame being made to CAPTURE_SKIP after that one.
	cap.skip_from = cpuprof_last.serial + 1;
	cap.skip_to = cpuprof_last.serial + 2 + CAPTURE_SKIP;

	Q_snprintf(cap.banner, sizeof(cap.banner),
		"CAPTURE %d/2 (%s): %s %.2f ms, over %.2f (frame to frame %.2f, cpu work %.2f, gpu %.2f); dip %.2f s, %d slow frames",
		nth, nth == 1 ? "first slow frame" : "worst frame of the dip",
		cap.worst_gpu >= cap.worst_cpu ? "gpu" : "cpu work", cap.worst_value, cap.threshold_ms,
		cap.worst_frame, cap.worst_cpu, cap.worst_gpu, (cap.dip_ms - cap.good_ms) * 1e-3f, cap.dip_frames);
	cap.banner_now = true;

	// the numbers, for the log - and not over the overlay, as the console's notify
	// lines (Matt 11-14: they printed over the capture banner)
	Con_SkipNotify(true);
	Com_Printf("PROFCAPTURE %s screenshots/%s: %s\n", which, name, cap.banner);
	for (int c = NUM_CATS - 1; c >= 0; c--)
		Com_Printf("PROFCAPTURE   %-18s %7.2f ms\n", cat_info[c].name, cap.held_cat[c]);
	const profiler_frame_extras_t *x = &cap.held_extras;
	if (x->cpu_submit_ms > 0.f)
		Com_Printf("PROFCAPTURE   cpu, this frame's R_BeginFrame to submit %7.2f ms\n", x->cpu_submit_ms);
	if (x->gpu_gap_ms > 0.f)
		Com_Printf("PROFCAPTURE   gpu idle between frames, before this one %7.2f ms\n", x->gpu_gap_ms);
	if (x->gap_ms > 0.f)
	{
		char gap[160];
		gap_text(gap, sizeof(gap), x);
		Com_Printf("PROFCAPTURE   list time no row covers %7.2f ms; %s\n", x->gap_ms, gap);
	}
	for (int i = 0; i < NUM_PROFILER_ENTRIES; i++)
	{
		const double ms = result_ms(cap.held, i);
		if (ms > 0.005)
			Com_Printf("PROFCAPTURE     %-34s %7.2f ms\n", entry_name(i), ms);
	}
	// the CPU's frame by section, and what the blood work was
	Com_Printf("PROFCAPTURE   cpu by section (the same frame):\n");
	for (int r = 0; r < NUM_CPUROWS; r++)
		if (cap.held_cpu_rows[r] >= 0.01f || cpurow_info[r].depth <= 0)
			Com_Printf("PROFCAPTURE     %*s%-*s %7.2f ms\n", 2 * max(cpurow_info[r].depth, 0), "",
				30 - 2 * max(cpurow_info[r].depth, 0), cpurow_info[r].name, cap.held_cpu_rows[r]);
	char line1[160], line2[160];
	blood_count_text(line1, line2, sizeof(line1), cap.held_counts);
	Com_Printf("PROFCAPTURE   %s\nPROFCAPTURE   %s\n", line1, line2);
	Con_SkipNotify(false);
}

static void
capture_frame(float cpu_ms, float busy_ms, float gpu_ms, int presents, uint32_t serial, const float *rows)
{
	if (!cap.on)
	{
		cap.on = Cvar_Get("profiler_capture", "0", 0);
		cap.fps = Cvar_Get("profiler_capture_fps", "0", 0);
	}

	cap.banner_now = false;
	if (cap.release)
	{
		cap.hold = false;
		cap.release = false;
	}

	if (!cvar_profiler || cvar_profiler->integer <= 0 || !cap.on->integer)
	{
		cap.in_dip = cap.hold = cap.release = false;
		cap.skip = 0;
		cap.skip_from = cap.skip_to = 0;
		cap.status[0] = 0;
		return;
	}

	const unsigned now_ms = Sys_Milliseconds();
	if (level_loaded || Com_LoadActive())
	{
		level_loaded = false;
		cap.quiet = CAPTURE_LOAD_QUIET;
		cap.quiet_until = now_ms + CAPTURE_LOAD_QUIET_MS;
	}
	if (cap.quiet > 0 || (cap.quiet_until && (int)(cap.quiet_until - now_ms) > 0))
	{
		if (cap.quiet > 0)
			cap.quiet--;
		cap.in_dip = cap.hold = cap.release = false;
		Q_snprintf(cap.status, sizeof(cap.status), "capture: a map is loading - armed again a second after it");
		return;
	}

	/* A CAPTURE'S OWN FRAMES ARE NOT MEASURED (11-14). The HDR screenshot reads the
	   frame back on the main thread - a 66 MB readback made, the GPU waited idle, 25M
	   halves to floats: 40-130 ms in "net, demo, predict", where the command runs -
	   and the frame after it starts on an empty GPU, idle while it is recorded. Those
	   frames counted on the GPU's time alone, which that idle made slow: they were
	   every "worst frame" of Matt's run, and three captures in a row set each other
	   off. Now they are neither slow nor good. */
	/* JUDGED BY THE FRAME'S OWN WORK (11-14; Matt: "fix the capture shot so it
	   captures the right numbers"). By its frame to frame time, the capture held
	   frames of 17-20 ms whose own CPU work (4-11 ms) and GPU time (13-16 ms) were
	   both under the line - the rest waiting, on the swapchain or a fence, which
	   showed nothing over. Now a frame is slow when its CPU work or its GPU time is
	   over: the frame that did the work is the one held. A frame slow only from
	   waiting says on what in the log (below), and takes no screenshot. */
	float value = max(busy_ms, gpu_ms);
	const bool skipped = serial
		? (cap.skip_from && serial - cap.skip_from <= cap.skip_to - cap.skip_from)
		: cap.skip > 0;
	if (cap.skip > 0)
		cap.skip--;
	if (skipped)
		return;
	value /= (float)max(presents, 1);

	cap.threshold_ms = 1000.f / capture_threshold_fps();
	const bool slow = value > cap.threshold_ms;

	// slow from waiting alone: which wait, at most once a second
	if (!slow && cpu_ms / (float)max(presents, 1) > cap.threshold_ms && rows)
	{
		static unsigned last_ms;
		const unsigned now = Sys_Milliseconds();
		if (now - last_ms >= 1000)
		{
			last_ms = now;
			Con_SkipNotify(true);
			Com_Printf("PROFCAPTURE waited, no capture: frame to frame %.2f ms, cpu work %.2f, gpu %.2f; "
				"fps limiter %.2f, reflex %.2f, swapchain %.2f, gpu fence %.2f, present %.2f\n",
				cpu_ms, busy_ms, gpu_ms, rows[CPUROW_WAIT_LIMITER], rows[CPUROW_WAIT_REFLEX],
				rows[CPUROW_WAIT_SWAPCHAIN], rows[CPUROW_WAIT_FENCE], rows[CPUROW_WAIT_PRESENT]);
			Con_SkipNotify(false);
		}
	}

	if (!cap.in_dip)
	{
		if (slow)
		{
			cap.in_dip = true;
			cap.worst_done = false;
			cap.dip_frames = 1;
			cap.dip_ms = cpu_ms;
			cap.good_ms = 0.f;
			cap.first_value = value;
			capture_hold_latest(value, cpu_ms, busy_ms, gpu_ms);
			capture_take("first", 1);
		}
	}
	else
	{
		cap.dip_ms += cpu_ms;
		if (slow)
		{
			cap.good_ms = 0.f;
			cap.dip_frames++;
			if (!cap.worst_done && value > cap.worst_value)
				capture_hold_latest(value, cpu_ms, busy_ms, gpu_ms);
		}
		else
		{
			cap.good_ms += cpu_ms;
		}

		const bool back = cap.good_ms >= CAPTURE_REARM_MS;
		if (!cap.worst_done && (back || cap.dip_ms >= CAPTURE_MAX_HOLD_MS))
		{
			if (cap.worst_value > cap.first_value)
				capture_take("worst", 2);
			else
			{
				Con_SkipNotify(true);
				Com_Printf("PROFCAPTURE the dip's first slow frame was its worst - no second capture\n");
				Con_SkipNotify(false);
			}
			cap.worst_done = true;
			cap.release = true;	// the held frame stays up for this frame's overlay
		}
		if (back)
			cap.in_dip = false;
	}

	if (cap.hold && !cap.release)
		Q_snprintf(cap.status, sizeof(cap.status),
			"capture: over %.2f ms - holding the dip's worst frame, %.2f ms; captured once back under for %.1f s",
			cap.threshold_ms, cap.worst_value, CAPTURE_REARM_MS * 1e-3f);
	else if (cap.in_dip)
		Q_snprintf(cap.status, sizeof(cap.status), "capture: dip captured - re-arming once back under %.2f ms for %.1f s",
			cap.threshold_ms, CAPTURE_REARM_MS * 1e-3f);
	else
		Q_snprintf(cap.status, sizeof(cap.status), "capture: armed - cpu work or gpu over %.2f ms takes a screenshot",
			cap.threshold_ms);
}

/* ONE FRAME A SAMPLE (Matt 11-14: "The CPU work and GPU work don't match the left
   and right"). The GPU's results are a frame from MAX_FRAMES_IN_FLIGHT ago; the frame
   time was this call's distance from the last one, and the CPU by section whichever
   frame closed last - three frames, so a capture showed one frame's GPU time beside
   another's CPU. serial names the CPU frame the results are (cpuprof.h's
   CPUPROF_RING): its own time close to close, its sections and its waits go with
   them, and "cpu frame time" is its "cpu work (sum)". 0 or gone from the ring:
   as before. */
static void
graph_record(uint32_t serial)
{
	const uint64_t now = Sys_Microseconds();
	float ms = graph.last_us ? (float)(now - graph.last_us) * 1e-3f : 0.f;
	const float waited = graph.last_us ? (float)(cpuprof_wait_us - graph.last_wait_us) * 1e-3f : 0.f;
	graph.last_us = now;
	graph.last_wait_us = cpuprof_wait_us;
	float busy = max(ms - waited, 0.f);

	const cpuprof_frame_t *f = NULL;
	if (serial && serial != graph.last_serial && cpuprof_ring[serial % CPUPROF_RING].serial == serial)
		f = &cpuprof_ring[serial % CPUPROF_RING];
	graph.last_serial = serial;
	float rows[NUM_CPUROWS];
	uint32_t counts[CPUPROF_COUNTER_COUNT];
	cpu_rows_of(f ? f : &cpuprof_last, rows, counts);
	if (f)
	{
		ms = (float)f->wall_us * 1e-3f;
		busy = rows[CPUROW_TOTAL];
	}
	else
		serial = 0;
	if (ms <= 0.f)
		return;
	// Two seconds or more is a level load, a vid_restart or the window put away:
	// start over, rather than carry it in the averages for the next ten seconds.
	if (ms >= 2000.f)
	{
		graph.count = graph.next = 0;
		graph.frame_top = graph.fps_top = graph.cat_top = 0.f;
		graph.text_ms = 0;
		// a dip in progress ends here too (its frames are gone); the frames
		// after a load are judged afresh
		cap.in_dip = cap.hold = cap.release = false;
		cap.status[0] = 0;
		return;
	}
	// what the renderer last reported (the fps counter's "presented")
	const int presents = vkpt_refdef.fd ? vkpt_refdef.fd->feedback.presented_frames : 1;
	const float gpu_ms = (float)vkpt_get_profiler_result(PROFILER_GPU_FRAME);
	graph.frame_ms[graph.next] = ms;
	graph.cpu_ms[graph.next] = busy;
	graph.gpu_ms[graph.next] = gpu_ms;
	graph.presents[graph.next] = (uint8_t)max(1, min(presents, 255));
	categories_of(profiler_data.query_pool_results, graph.cat_ms[graph.next]);
	graph.gap_ms[graph.next] = frame_extras.gpu_gap_ms;
	graph.shot[graph.next] = serial && cap.skip_from && serial - cap.skip_from <= cap.skip_to - cap.skip_from;
	memcpy(graph.cpu_rows[graph.next], rows, sizeof(rows));
	memcpy(graph.counts[graph.next], counts, sizeof(counts));
	graph.next = (graph.next + 1) % GRAPH_SAMPLES;
	graph.count = min(graph.count + 1, GRAPH_SAMPLES);
	graph_scale(&graph.frame_top, GRAPH_FRAME_TOP, graph_frame_value);
	graph_scale(&graph.fps_top, GRAPH_FPS_TOP, graph_fps_value);
	graph_scale(&graph.cat_top, GRAPH_FRAME_TOP, graph_cat_value);

	capture_frame(ms, busy, gpu_ms, presents, serial, rows);
}

VkResult
vkpt_profiler_initialize()
{
	memset(&profiler_data, 0, sizeof(profiler_data));

	VkQueryPoolCreateInfo query_pool_info = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = MAX_FRAMES_IN_FLIGHT * NUM_PROFILER_QUERIES_PER_FRAME,
	};
	vkCreateQueryPool(qvk.device, &query_pool_info, NULL, &profiler_data.query_pool);
	return VK_SUCCESS;
}

VkResult
vkpt_profiler_destroy()
{
	vkDestroyQueryPool(qvk.device, profiler_data.query_pool, NULL);
	vkpt_profiler_release();
	return VK_SUCCESS;
}

/* Nothing left behind for the next renderer: it used to keep the freed sample
   buffers and their count, so a renderer that took results without making its
   own pool first (D3D12) would have written into freed memory. */
void
vkpt_profiler_release(void)
{
	for (int i = 0; i < NUM_PROFILER_ENTRIES; i++)
		Z_Free(profiler_data.samples[i].data);
	memset(&profiler_data, 0, sizeof(profiler_data));
	upload_spikes_reset();
	memset(&frame_extras, 0, sizeof(frame_extras));
	frame_extras.gap_after = frame_extras.gap_before = -1;
}

static inline void
remove_excess_samples(profiler_entry_samples_t *entry_samples, size_t max_samples)
{
	if (entry_samples->num_samples <= max_samples)
		return;

	// Compute the index of the 'oldest' value in the sample data buffer...
	size_t oldest_idx = (entry_samples->next_idx + profiler_data.allocated_samples - entry_samples->num_samples) % profiler_data.allocated_samples;
	do {
		// ...and keep on removing the oldest data as long the count is above max_samples
		entry_samples->accumulated -= entry_samples->data[oldest_idx];
		entry_samples->num_samples--;
		oldest_idx = (oldest_idx + 1) % profiler_data.allocated_samples;
	} while (entry_samples->num_samples > max_samples);
}

// Change size of profiler samples circular buffer
static void
set_sample_count(size_t new_samples)
{
	size_t old_allocated_samples = profiler_data.allocated_samples;
	for (int i = 0; i < NUM_PROFILER_ENTRIES; i++)
	{
		// Make sure entry has at most as much samples as the new buffer size
		profiler_entry_samples_t *entry_samples = &profiler_data.samples[i];
		remove_excess_samples(entry_samples, new_samples);

		uint64_t* new_sample_data = Z_Malloc(new_samples * sizeof(uint64_t));
		if(entry_samples->num_samples > 0)
		{
			size_t oldest_idx = (entry_samples->next_idx + profiler_data.allocated_samples - entry_samples->num_samples) % profiler_data.allocated_samples;
			// Reassemble sample data at start of new buffer
			if (oldest_idx >= entry_samples->next_idx)
			{
				// Sample data wraps around
				uint64_t *dest = new_sample_data;
				size_t count = old_allocated_samples - oldest_idx;
				assert(count + entry_samples->next_idx <= new_samples);
				memcpy(dest, entry_samples->data + oldest_idx, count * sizeof(uint64_t));
				dest += count;
				memcpy(dest, entry_samples->data, entry_samples->next_idx * sizeof(uint64_t));
				count += entry_samples->next_idx;
				assert(count <= entry_samples->num_samples);
				entry_samples->next_idx = count % new_samples;
			}
			else
			{
				// No wraparound
				assert(entry_samples->num_samples <= new_samples);
				memcpy(new_sample_data, entry_samples->data + oldest_idx, entry_samples->num_samples * sizeof(uint64_t));
				entry_samples->next_idx = entry_samples->num_samples % new_samples;
			}
		}
		else
		{
			entry_samples->next_idx = 0;
		}
		Z_Free(entry_samples->data);
		entry_samples->data = new_sample_data;
	}
	profiler_data.allocated_samples = new_samples;
}

// Record a single profiler sample value
static inline void
record_sample(int idx, uint64_t value)
{
	profiler_entry_samples_t *entry_samples = &profiler_data.samples[idx];
	remove_excess_samples(entry_samples, profiler_data.allocated_samples - 1);
	entry_samples->data[entry_samples->next_idx] = value;
	entry_samples->accumulated += value;
	entry_samples->num_samples++;
	entry_samples->next_idx = (entry_samples->next_idx + 1) % profiler_data.allocated_samples;
}

// Reset accumulated profiler samples
static inline void
reset_samples(int idx)
{
	profiler_entry_samples_t *entry_samples = &profiler_data.samples[idx];
	entry_samples->accumulated = 0;
	entry_samples->num_samples = 0;
	entry_samples->next_idx = 0;
}

static void
profiler_timestamp(VkCommandBuffer cmd_buf, int idx, VKPTProfilerAction action, VkPipelineStageFlagBits stage)
{
	idx = idx * 2 + action + qvk.current_frame_index * NUM_PROFILER_QUERIES_PER_FRAME;

	set_current_gpu(cmd_buf, 0);

	vkCmdWriteTimestamp(cmd_buf, stage, profiler_data.query_pool, idx);

	set_current_gpu(cmd_buf, ALL_GPUS);

	profiler_data.queries_used[idx] = true;
}

VkResult
vkpt_profiler_query(VkCommandBuffer cmd_buf, int idx, VKPTProfilerAction action)
{
	VkPipelineStageFlagBits stage = (action == PROFILER_START) 
		? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT 
		: VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

	profiler_timestamp(cmd_buf, idx, action, stage);

	return VK_SUCCESS;
}

/* For work the renderer does not record itself - NVIDIA Flow submits its own.
   Its start goes in early in the frame, while the frame before may still be
   running; at the top of the pipe it would be charged with the rest of that. */
VkResult
vkpt_profiler_query_after(VkCommandBuffer cmd_buf, int idx, VKPTProfilerAction action)
{
	profiler_timestamp(cmd_buf, idx, action, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	return VK_SUCCESS;
}

VkResult 
vkpt_reset_query_pool(VkCommandBuffer cmd_buf) {
	vkCmdResetQueryPool(cmd_buf, profiler_data.query_pool,
		NUM_PROFILER_QUERIES_PER_FRAME * qvk.current_frame_index,
		NUM_PROFILER_QUERIES_PER_FRAME);

	return VK_SUCCESS;
}

// Resize sample buffers, if necessary
static void
resize_samples(void)
{
	size_t new_samples = max(cvar_profiler_samples->integer, 1);
	if (profiler_data.allocated_samples != new_samples)
		set_sample_count(new_samples);
}

/* A frame's results, in query_pool_results: a sample for every entry whose start
   was written (used[entry * 2]), the others' averages started over. Both
   renderers'. */
static void
take_results(const bool *used)
{
	for (int idx = 0; idx < NUM_PROFILER_ENTRIES; idx++)
	{
		if (!used[idx * 2])
		{
			profiler_data.query_pool_results[idx * 2 + 0] = 0;
			profiler_data.query_pool_results[idx * 2 + 1] = 0;
			reset_samples(idx);
		}
		else
		{
			uint64_t begin = profiler_data.query_pool_results[idx * 2 + 0];
			uint64_t end = profiler_data.query_pool_results[idx * 2 + 1];

			if (begin != 0 && end != 0)
				record_sample(idx, end - begin);
			else
				reset_samples(idx);
		}
	}

	double upload_ms = vkpt_get_profiler_result(PROFILER_UPLOAD_LIGHTS);
	if (upload_ms > 0.0)
		upload_spikes_record((float)upload_ms, (float)vkpt_get_profiler_result(PROFILER_FRAME_TIME));
}

void
vkpt_profiler_take_results(const uint64_t *ticks, const bool *used, double ns_per_tick)
{
	profiler_data.d3d12 = true;
	qvk.timestampPeriod = (float)ns_per_tick;
	resize_samples();

	bool any_queries_used = false;
	for (int idx = 0; idx < NUM_PROFILER_QUERIES_PER_FRAME && !any_queries_used; idx++)
		any_queries_used = used[idx];

	if (any_queries_used)
	{
		memcpy(profiler_data.query_pool_results, ticks, sizeof(profiler_data.query_pool_results));
		take_results(used);
	}
	else
	{
		memset(profiler_data.query_pool_results, 0, sizeof(profiler_data.query_pool_results));
	}

	graph_record(frame_extras.cpu_serial);
}

// the CPU frame each query slot's frame closes as (cpuprof.h's CPUPROF_RING)
static uint32_t vk_slot_serial[MAX_FRAMES_IN_FLIGHT];

VkResult
vkpt_profiler_next_frame(VkCommandBuffer cmd_buf, qboolean secondPass)
{
	resize_samples();

	bool any_queries_used = false;
	int profileQueriesPerframe = NUM_PROFILER_QUERIES_PER_FRAME;

	for (int idx = 0; idx < NUM_PROFILER_QUERIES_PER_FRAME; idx++)
	{
		int queryIndex = idx + qvk.current_frame_index * NUM_PROFILER_QUERIES_PER_FRAME;

		if (profiler_data.queries_used[queryIndex])
		{
			any_queries_used = true;
			break;
		}
	}

	if (any_queries_used)
	{
		//memset(profiler_data.query_pool_results, 0, sizeof(profiler_data.query_pool_results));

		VkResult result = vkGetQueryPoolResults(qvk.device, profiler_data.query_pool,
			NUM_PROFILER_QUERIES_PER_FRAME * qvk.current_frame_index,
			NUM_PROFILER_QUERIES_PER_FRAME,
			sizeof(profiler_data.query_pool_results),
			profiler_data.query_pool_results,
			sizeof(profiler_data.query_pool_results[0]),
			VK_QUERY_RESULT_64_BIT);

		if (result != VK_SUCCESS && result != VK_NOT_READY)
		{
			Com_EPrintf("Failed call to vkGetQueryPoolResults, error code = %d\n", result);
			any_queries_used = false;
		}
	}

	if (any_queries_used)
	{
		take_results(profiler_data.queries_used + qvk.current_frame_index * NUM_PROFILER_QUERIES_PER_FRAME);
	}
	else
	{
		memset(profiler_data.query_pool_results, 0, sizeof(profiler_data.query_pool_results));
	}

	graph_record(vk_slot_serial[qvk.current_frame_index]);
	vk_slot_serial[qvk.current_frame_index] = cpuprof_last.serial + 1;	// the frame starting now

	vkCmdResetQueryPool(cmd_buf, profiler_data.query_pool,
			NUM_PROFILER_QUERIES_PER_FRAME * qvk.current_frame_index, 
			NUM_PROFILER_QUERIES_PER_FRAME);

	//if (secondPass) {
		memset(profiler_data.queries_used + qvk.current_frame_index * NUM_PROFILER_QUERIES_PER_FRAME, 0, sizeof(bool) * NUM_PROFILER_QUERIES_PER_FRAME);
	//}
	

	return VK_SUCCESS;
}

static void
draw_query(int x, int y, qhandle_t font, const char *enum_name, int idx)
{
	char buf[256];
	int i;
	for(i = 0; i < LENGTH(buf) - 1 && enum_name[i]; i++)
		buf[i] = enum_name[i] == '_' ? ' ' : (char)tolower(enum_name[i]); 
	buf[i] = 0;

	R_DrawString(x, y, 0, 128, buf, font);
	// the held frame while a capture holds one (profiler_capture)
	double ms = result_ms(shown_results(), idx);
	double avg_ms = ((double)profiler_data.samples[idx].accumulated / (profiler_data.samples[idx].num_samples * 1e6)) * qvk.timestampPeriod;

	if(ms > 0.005)
		snprintf(buf, sizeof buf, "%8.2f ms %8.2f ms", ms, avg_ms);
	else if(avg_ms > 0.005)
		snprintf(buf, sizeof buf, "       N/A  %8.2f ms", avg_ms);
	else
		snprintf(buf, sizeof buf, "       N/A");

	R_DrawString(x + 256, y, 0, 128, buf, font);
}

/* THE ORDER THEY RUN IN (Matt, 2026-11-07): below the two whole-frame rows, the
   table is sorted by where each pass STARTED on the GPU, relative to the start of
   the frame, in the latest frame that timed it - so it reads top to bottom as the
   frame runs, on either renderer (vkpt and D3D12 order their passes differently),
   and stays right when a pass moves. A pass that did not run this frame keeps the
   place it had; one never timed yet goes last, as listed. A sub-pass that starts
   with its parent comes after it (the longer first). A pass the D3D12 renderer
   times in two pieces (its shadow maps, BVH, Flow) sits where the first starts. */
typedef struct {
	int idx;			// PROFILER_*
	int listed;			// its place in draw_profiler's list, the last tie-break
	const char *name;
} profiler_row_t;

// ticks from the frame's start, of the latest frame that timed each entry
static int64_t row_start[NUM_PROFILER_ENTRIES], row_end[NUM_PROFILER_ENTRIES];
static bool row_seen[NUM_PROFILER_ENTRIES];
static bool row_now[NUM_PROFILER_ENTRIES];	// timed in this frame's results

static void note_row_times(void)
{
	const uint64_t *r = shown_results();
	const uint64_t frame = r[PROFILER_GPU_FRAME * 2] ? r[PROFILER_GPU_FRAME * 2] : r[PROFILER_FRAME_TIME * 2];
	memset(row_now, 0, sizeof(row_now));
	if (!frame)
		return;
	for (int i = 0; i < NUM_PROFILER_ENTRIES; i++)
	{
		const uint64_t b = r[i * 2], e = r[i * 2 + 1];
		if (b && e >= b)
		{
			// before the frame's start (vkpt's transfer queue) comes out negative
			row_start[i] = (int64_t)(b - frame);
			row_end[i] = (int64_t)(e - frame);
			row_seen[i] = true;
			row_now[i] = true;
		}
	}
}

static bool row_before(const profiler_row_t *a, const profiler_row_t *b)
{
	const bool sa = row_seen[a->idx], sb = row_seen[b->idx];
	if (sa != sb)
		return sa;
	if (sa && row_start[a->idx] != row_start[b->idx])
		return row_start[a->idx] < row_start[b->idx];
	if (sa && row_end[a->idx] != row_end[b->idx])
		return row_end[a->idx] > row_end[b->idx];
	return a->listed < b->listed;
}

/* A ROW WITH NO TIME THIS FRAME (N/A: a pass that skipped this frame) has only
   the start it had the last time it ran, while the passes round it start a
   little earlier or later from frame to frame. Sorted by that old start, it
   swapped places with a neighbour that starts at about the same time, back and
   forth (Matt, 11-12: flow smoke lighting hopping over fog froxel scatter -
   "is order of operations constantly changing?" It is not; only the sort was).
   So only the rows timed this frame are sorted by when they started, and each
   of the others goes back in right after the row it was drawn after last
   time - in front of the table if nothing was. A row never drawn yet goes where
   its last start puts it, or after the row listed before it if it never ran. */
static int drawn_order[NUM_PROFILER_ENTRIES];	// PROFILER_* as drawn last time
static int drawn_count;

static int drawn_place(int idx)
{
	for (int i = 0; i < drawn_count; i++)
		if (drawn_order[i] == idx)
			return i;
	return -1;
}

static void order_rows(profiler_row_t *rows, int n)
{
	profiler_row_t out[NUM_PROFILER_ENTRIES], rest[NUM_PROFILER_ENTRIES];
	int rest_place[NUM_PROFILER_ENTRIES];
	int m = 0, k = 0;

	for (int i = 0; i < n; i++)
	{
		if (row_now[rows[i].idx])
		{
			// insertion sort: a few dozen rows, nearly in order already
			int j = m++;
			for (; j > 0 && row_before(&rows[i], &out[j - 1]); j--)
				out[j] = out[j - 1];
			out[j] = rows[i];
		}
		else
		{
			// the rest in the order they were drawn, those never drawn after, as listed
			const int place = drawn_place(rows[i].idx);
			const int key = place >= 0 ? place : NUM_PROFILER_ENTRIES + rows[i].listed;
			int j = k++;
			for (; j > 0 && key < rest_place[j - 1]; j--)
			{
				rest[j] = rest[j - 1];
				rest_place[j] = rest_place[j - 1];
			}
			rest[j] = rows[i];
			rest_place[j] = key;
		}
	}

	for (int i = 0; i < k; i++)
	{
		int at;
		if (rest_place[i] < NUM_PROFILER_ENTRIES)
		{
			// after the nearest row before it last time that is in the table now
			at = 0;
			for (int p = rest_place[i] - 1; p >= 0 && !at; p--)
			{
				for (int j = 0; j < m; j++)
				{
					if (out[j].idx == drawn_order[p])
					{
						at = j + 1;
						break;
					}
				}
			}
		}
		else if (row_seen[rest[i].idx])
		{
			at = m;
			while (at > 0 && row_before(&rest[i], &out[at - 1]))
				at--;
		}
		else
		{
			// never ran: after the row listed nearest before it
			at = 0;
			int best = -1;
			for (int j = 0; j < m; j++)
			{
				if (out[j].listed < rest[i].listed && out[j].listed > best)
				{
					best = out[j].listed;
					at = j + 1;
				}
			}
		}
		memmove(&out[at + 1], &out[at], (size_t)(m - at) * sizeof(out[0]));
		out[at] = rest[i];
		m++;
	}

	memcpy(rows, out, (size_t)n * sizeof(rows[0]));
	for (int i = 0; i < n; i++)
		drawn_order[i] = rows[i].idx;
	drawn_count = n;
}

// The graphs' colours: frame time amber, the GPU's green, fps blue, frame
// generation's presented fps violet
#define GRAPH_FRAME	MakeColor(255, 200, 60, 255)
#define GRAPH_GPU	MakeColor(90, 220, 90, 255)
#define GRAPH_FPS	MakeColor(90, 170, 255, 255)
#define GRAPH_FG	MakeColor(225, 120, 255, 255)
// profiler 3-6's capture line (profiler_capture_fps, or the display's rate)
#define GRAPH_CAPTURE	MakeColor(255, 255, 255, 190)

static int
graph_cmp(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

/* A series as a line, a sample a column of cw pixels with the newest at the right
   edge, each column joined to the one before. A value over the top runs along it; 0
   (not measured) is a gap. */
static void
graph_line(int left, int top, int w, int h, int thick, int cw, const float *v, int n, float vmax,
	uint32_t color)
{
	int prev = -1;
	for (int i = 0; i < n; i++)
	{
		if (!(v[i] > 0.f))
		{
			prev = -1;
			continue;
		}
		const float f = v[i] / vmax;
		const int y = top + (int)((1.f - min(f, 1.f)) * (float)(h - thick));
		const int y0 = prev >= 0 ? min(y, prev) : y;
		const int y1 = prev >= 0 ? max(y, prev) : y;
		R_DrawFill32(left + w - (n - i) * cw, y0, cw, y1 - y0 + thick, color);
		prev = y;
	}
}

// the panel behind a graph, with lines at its quarters
static void
graph_panel(int left, int top, int w, int h)
{
	R_DrawFill32(left, top, w, h, MakeColor(0, 0, 0, 150));
	for (int i = 1; i < 4; i++)
		R_DrawFill32(left, top + h * i / 4, w, 1, MakeColor(255, 255, 255, i == 2 ? 70 : 35));
}

static void
graph_text(float scale, int px, int py, uint32_t color, const char *s, qhandle_t font)
{
	R_SetColor(color);
	R_DrawString((int)((float)px * scale), (int)((float)py * scale), 0, 128, s, font);
}

/* profiler 2. Two panels where the table goes: frame time (the CPU's frame to
   frame, and the GPU's whole frame under it) and fps, with their latest, average
   and worst values over what the panel shows. The lines are drawn in pixels, a
   sample a column; the text in the table's scale. */
static void
draw_graphs(float scale, qhandle_t font)
{
	const float u = 1.f / scale;			// pixels a text unit (a character is 8)
	const int left = 500, top = 100;		// where the table starts, in pixels
	const int w = (int)(352.f * u), h = (int)(64.f * u), row = (int)(10.f * u);
	const int thick = max(1, (int)(0.5f * u));	// 2 pixels at 4K
	const int n = min(graph.count, min(w, GRAPH_SAMPLES));

	static float frame[GRAPH_SAMPLES], gpu[GRAPH_SAMPLES], fps[GRAPH_SAMPLES], fg[GRAPH_SAMPLES];
	static float sorted[GRAPH_SAMPLES];
	double sum = 0.0, gpu_sum = 0.0, presented = 0.0;
	int gpu_n = 0, fg_n = 0;
	float gpu_max = 0.f, gpu_last = 0.f;
	for (int i = 0; i < n; i++)
	{
		const int k = (graph.next - n + i + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		frame[i] = sorted[i] = graph.frame_ms[k];
		fps[i] = 1000.f / frame[i];
		// frame generation's output: rendered fps times the presents each made;
		// a gap (0) where it was off
		fg[i] = graph.presents[k] > 1 ? fps[i] * (float)graph.presents[k] : 0.f;
		fg_n += graph.presents[k] > 1;
		presented += max(graph.presents[k], 1);
		gpu[i] = graph.gpu_ms[k];
		sum += frame[i];
		if (gpu[i] > 0.f)
		{
			gpu_sum += gpu[i];
			gpu_n++;
			gpu_max = max(gpu_max, gpu[i]);
			gpu_last = gpu[i];
		}
	}
	if (n > 0)
		qsort(sorted, n, sizeof(sorted[0]), graph_cmp);
	const float gpu_avg = gpu_n ? (float)(gpu_sum / gpu_n) : 0.f;
	// graph_scale keeps them; until the first sample, the defaults
	graph.frame_top = max(graph.frame_top, GRAPH_FRAME_TOP);
	graph.fps_top = max(graph.fps_top, GRAPH_FPS_TOP);

	const unsigned now = Sys_Milliseconds();
	if (n > 0 && (graph.text_ms == 0 || now - graph.text_ms >= 500))
	{
		graph.text_ms = now;
		const float last = frame[n - 1], avg = (float)(sum / n);
		// 1% low: the frame time 99% of frames beat, as an fps
		const float p99 = sorted[(int)(0.99f * (float)(n - 1))], worst = sorted[n - 1];
		const int per_render = graph.presents[(graph.next - 1 + GRAPH_SAMPLES) % GRAPH_SAMPLES];
		Q_snprintf(graph.text[0], sizeof(graph.text[0]), "frame %6.2f ms   avg %6.2f   max %6.1f", last, avg, worst);
		if (gpu_n)
			Q_snprintf(graph.text[1], sizeof(graph.text[1]), "gpu   %6.2f ms   avg %6.2f   max %6.1f", gpu_last, gpu_avg, gpu_max);
		else
			Q_strlcpy(graph.text[1], "gpu   not timed", sizeof(graph.text[1]));
		Q_snprintf(graph.text[2], sizeof(graph.text[2]), "fps   %6.0f      avg %6.0f   1%% low %4.0f",
			1000.f / last, 1000.f / avg, 1000.f / p99);
		// frame generation's row while any of the panel had it; the average is every
		// present over the panel's time, rendered frames counted once where it was off
		if (fg_n)
			Q_snprintf(graph.text[3], sizeof(graph.text[3]), "fg %dx %6.0f      avg %6.0f   presented",
				max(per_render, 1), 1000.f / last * (float)max(per_render, 1), presented * 1000.0 / sum);
		else
			graph.text[3][0] = 0;
		Q_snprintf(graph.text[4], sizeof(graph.text[4]), "the last %.1f s, newest on the right", sum * 1e-3);
	}

	// frame time: two text rows, then its panel
	int y = top;
	const int frame_box = y + 2 * row;
	const int fps_text = frame_box + h + row / 2;
	const int fps_box = fps_text + (graph.text[3][0] ? 2 : 1) * row;

	R_SetScale(1.0f);
	graph_panel(left, frame_box, w, h);
	graph_panel(left, fps_box, w, h);
	if (n > 0)
	{
		graph_line(left, frame_box, w, h, thick, 1, gpu, n, graph.frame_top, GRAPH_GPU);
		graph_line(left, frame_box, w, h, thick, 1, frame, n, graph.frame_top, GRAPH_FRAME);
		if (fg_n)
			graph_line(left, fps_box, w, h, thick, 1, fg, n, graph.fps_top, GRAPH_FG);
		graph_line(left, fps_box, w, h, thick, 1, fps, n, graph.fps_top, GRAPH_FPS);
	}

	R_SetScale(scale);
	const uint32_t grey = MakeColor(170, 170, 170, 255);
	graph_text(scale, left, y, GRAPH_FRAME, graph.text[0], font);
	graph_text(scale, left, y + row, GRAPH_GPU, graph.text[1], font);
	graph_text(scale, left, fps_text, GRAPH_FPS, graph.text[2], font);
	if (graph.text[3][0])
		graph_text(scale, left, fps_text + row, GRAPH_FG, graph.text[3], font);
	graph_text(scale, left, fps_box + h + row / 4, grey, graph.text[4], font);

	// each panel's scale, at its top and middle lines, to its right
	char buf[32];
	const int label_x = left + w + (int)(4.f * u);
	Q_snprintf(buf, sizeof(buf), "%.1f ms", graph.frame_top);
	graph_text(scale, label_x, frame_box, grey, buf, font);
	Q_snprintf(buf, sizeof(buf), "%.1f", graph.frame_top * 0.5f);
	graph_text(scale, label_x, frame_box + h / 2 - (int)(4.f * u), grey, buf, font);
	Q_snprintf(buf, sizeof(buf), "%g fps", graph.fps_top);
	graph_text(scale, label_x, fps_box, grey, buf, font);
	Q_snprintf(buf, sizeof(buf), "%g", graph.fps_top * 0.5f);
	graph_text(scale, label_x, fps_box + h / 2 - (int)(4.f * u), grey, buf, font);

	R_ClearColor();
	R_SetScale(1.0f);
}

/* One stacked column or bar, bottom up in category order, in pixels. Each
   category's top is placed from the running sum, so rounding never opens a gap
   or piles up; one under a pixel is not drawn. Each segment stops gap pixels
   short of the next, the panel showing between (any two neighbours apart even
   where their colours are close). The idle GPU is striped - across, a stripe
   pixels high, where stripe > 0 (the bars, the key); in the history, whose
   columns would take too many quads for that, every other column (phase) darker. */
static void
draw_stack(int x, int bw, int bottom, int h, float vtop, const float *v, int gap, int stripe, int phase)
{
	static uint32_t idle_dark;
	if (!idle_dark)
		idle_dark = display_color(80, 80, 80, 255);
	const uint32_t stripe_color = MakeColor(0, 0, 0, 140);
	float sum = 0.f;
	int below = bottom;
	for (int c = 0; c < NUM_CATS; c++)
	{
		if (!(v[c] > 0.f))
			continue;
		sum += v[c];
		const int y = bottom - (int)(min(sum / vtop, 1.f) * (float)h);
		if (y < below)
		{
			const int top = below - y > 3 * gap ? y + gap : y;
			const uint32_t color = c == CAT_IDLE && stripe <= 0 && (phase & 1) ? idle_dark : cat_color(c);
			R_DrawFill32(x, top, bw, below - top, color);
			if (c == CAT_IDLE && stripe > 0)
				for (int s = below - 2 * stripe; s > top; s -= 2 * stripe)
					R_DrawFill32(x, max(s, top), bw, min(stripe, below - max(s, top)), stripe_color);
		}
		below = y;
	}
}

// a key's swatch for category c: a square, striped for the idle GPU as in the bars
static void
draw_swatch(int x, int y, float u, int c)
{
	const int sw = max(3, (int)(7.f * u));
	float v[NUM_CATS] = { 0 };
	v[c] = 1.f;
	draw_stack(x, sw, y + sw, sw, 1.f, v, 0, c == CAT_IDLE ? max(1, (int)(0.75f * u)) : 0, 0);
}

// the capture line across a panel (profiler_capture_fps, or the display's rate)
static void
draw_capture_line(int left, int box, int w, int h, int thick, float vtop)
{
	const float line_ms = 1000.f / capture_threshold_fps();
	if (line_ms < vtop)
		R_DrawFill32(left, box + h - (int)(line_ms / vtop * (float)h), w, thick, GRAPH_CAPTURE);
}

/* profiler_capture's lines above whatever the mode draws: what it is doing, and on
   the frame a screenshot is of, which capture and its numbers. */
static void
draw_capture_text(float scale, qhandle_t font)
{
	if (!cap.on || !cap.on->integer)
		return;
	const float u = 1.f / scale;
	R_SetScale(scale);
	if (cap.status[0])
		graph_text(scale, 500, max(0, 100 - (int)(12.f * u)), MakeColor(255, 200, 60, 255), cap.status, font);
	if (cap.banner_now)
		graph_text(scale, 500, max(0, 100 - (int)(24.f * u)), MakeColor(255, 70, 70, 255), cap.banner, font);
	R_ClearColor();
	R_SetScale(1.0f);
}

/* The CPU's frame time over profiler 3-6's stack (Matt 11-15: dark green - amber read
   as flow's orange or direct lighting's yellow; darker than indirect lighting's green).
   The CPU key's "cpu work (sum)" is the same number, in the same colour. Profiler 2's
   frame time graph keeps GRAPH_FRAME's amber. The CPU's recording of the frame
   (R_BeginFrame to submit) is no longer drawn: the capture log still prints it. */
#define GRAPH_CPU	MakeColor(30, 125, 45, 255)

// what a key shows: per category, and the lines under them; [0] now (or held), [1] average, [2] worst
typedef struct {
	float cat[3][NUM_CATS];
	float total[3], cpu[3], frame[3];	// cpu: its own work; frame: frame to frame
	float gap[3];	// the GPU idle between frames, before this one
	float cpu_rows[3][NUM_CPUROWS];
	uint32_t counts[CPUPROF_COUNTER_COUNT];	// the shown frame's
	int columns;	// 3 with the worst, 2 without
} key_values_t;

// a row's running average over profiler_samples, as the table's (draw_query)
static double
avg_result_ms(int idx)
{
	const profiler_entry_samples_t *e = &profiler_data.samples[idx];
	return e->num_samples ? ((double)e->accumulated / ((double)e->num_samples * 1e6)) * qvk.timestampPeriod : 0.0;
}

/* A key's numbers, six characters each: two decimals to 999.99, whole ms past that
   (a load's hitch), so a column never grows into the next. */
static void
key_numbers(char *buf, size_t size, const float *v, int columns)
{
	size_t at = 0;
	buf[0] = 0;
	for (int i = 0; i < min(columns, 3) && at + 1 < size; i++)
	{
		const char *sep = i ? "  " : "";
		at += v[i] < 999.995f ? Q_scnprintf(buf + at, size - at, "%s%6.2f", sep, v[i])
			: Q_scnprintf(buf + at, size - at, "%s%6.0f", sep, v[i]);
	}
}

/* THE KEYS' LAYOUT (Matt 11-14: in profiler 3 the CPU key printed over the GPU key's
   max column, and "blas plan, instances" ran into its own numbers). The columns
   come from the names: each key's numbers start a little past its widest name, the
   CPU key a gap past the GPU key's numbers - a longer name moves them instead of
   running into them. In text units, a character 8. */
#define KEY_SWATCH_UNITS	11	// a GPU key row's swatch and the space after it
#define KEY_NAME_GAP		12	// the widest name to the numbers
#define KEY_GAP				32	// the GPU key's last column to the CPU key
#define LEGEND_GAP			80	// profiler 5/6: the panel to its legend, past the scale labels

// the GPU key's rows under the categories
enum { KEYROW_TOTAL, KEYROW_CPU, KEYROW_FRAME, KEYROW_GAP, KEYROW_SOUND, NUM_KEYROWS };
static const char *const key_row_names[NUM_KEYROWS] = {
	[KEYROW_TOTAL]  = "gpu frame (total)",
	[KEYROW_CPU]    = "cpu frame time",
	[KEYROW_FRAME]  = "frame to frame",
	[KEYROW_GAP]    = "gpu idle",
	[KEYROW_SOUND]  = "sound sim (own queue)",
};

static int
key_numbers_units(int columns)
{
	return 8 * (columns >= 3 ? 22 : 14);
}

static int
widest_cat_name(void)
{
	int most = 0;
	for (int c = 0; c < NUM_CATS; c++)
		most = max(most, (int)strlen(cat_info[c].name));
	return most;
}

// where the GPU key's numbers start, from its left
static int
gpu_key_num_units(void)
{
	int most = widest_cat_name();
	for (int r = 0; r < NUM_KEYROWS; r++)
		most = max(most, (int)strlen(key_row_names[r]));
	return KEY_SWATCH_UNITS + 8 * most + KEY_NAME_GAP;
}

// where the CPU key's numbers start, from its left: the widest name with its indent
static int
cpu_key_num_units(void)
{
	int most = 0;
	for (int r = 0; r < NUM_CPUROWS; r++)
		most = max(most, 6 * max(cpurow_info[r].depth, 0) + 8 * (int)strlen(cpurow_info[r].name));
	return most + KEY_NAME_GAP;
}

// the CPU key's left, from the GPU key's
static int
cpu_key_offset_units(int columns)
{
	return gpu_key_num_units() + key_numbers_units(columns) + KEY_GAP;
}

// both keys side by side
static int
keys_units(int columns)
{
	return cpu_key_offset_units(columns) + cpu_key_num_units() + key_numbers_units(columns);
}

// profiler 5/6's legend: a category's name, or the capture line's "60 fps (16.67 ms)"
static int
legend_units(void)
{
	return KEY_SWATCH_UNITS + 8 * max(widest_cat_name(), 18);
}

/* Where a panel starts: 500 pixels in, as the table does, or as far left as keeps
   width_units of it on the screen (both keys of profiler 3 are 760 units: 1520
   pixels at 1080p's 2x, past the right edge from 500). */
static int
panel_left(float u, int width_units)
{
	const int margin = (int)(8.f * u);
	const int room = r_config.width - (int)((float)width_units * u) - margin;
	return max(margin, min(500, room));
}

/* The key: a row a category, top of the stack first, its swatch in pixels and its
   name and numbers in the table's scale; the GPU frame's total; the CPU's frame time
   (dark green: its own work, the waits on the GPU, the swapchain, Reflex and the
   limiter taken out); frame to frame, the pace either one sets; the ray traced sound's last run, beside the frame on its
   own queue; and where in the frame's list "unaccounted" mostly is. */
// the GPU's key; returns the first row under it
static int
draw_key(float scale, qhandle_t font, int key_x, int key_y, int row, const key_values_t *k)
{
	const float u = 1.f / scale;
	const int thick = max(1, (int)(0.5f * u));
	const uint32_t white = MakeColor(255, 255, 255, 255), grey = MakeColor(170, 170, 170, 255);
	const double sound_now = result_ms(shown_results(), PROFILER_SOUND_TRACE);

	R_SetScale(1.0f);
	for (int j = 0; j < NUM_CATS; j++)
		draw_swatch(key_x, key_y + j * row + (int)u, u, NUM_CATS - 1 - j);
	const int sw = max(3, (int)(7.f * u));
	int y = key_y + (NUM_CATS + 1) * row + (int)(3.f * u);
	R_DrawFill32(key_x, y, sw, thick, GRAPH_CPU);

	R_SetScale(scale);
	char buf[192];
	const int name_x = key_x + (int)((float)KEY_SWATCH_UNITS * u), num_x = key_x + (int)((float)gpu_key_num_units() * u);
	const char *hold = cap.hold ? "  held" : "   now";
	Q_snprintf(buf, sizeof(buf), k->columns >= 3 ? "%s     avg     max" : "%s     avg", hold);
	graph_text(scale, num_x, key_y - row, grey, buf, font);
	for (int j = 0; j < NUM_CATS; j++)
	{
		const int c = NUM_CATS - 1 - j;
		const float v[3] = { k->cat[0][c], k->cat[1][c], k->cat[2][c] };
		graph_text(scale, name_x, key_y + j * row, white, cat_info[c].name, font);
		key_numbers(buf, sizeof(buf), v, k->columns);
		graph_text(scale, num_x, key_y + j * row, white, buf, font);
	}
	y = key_y + NUM_CATS * row;
	graph_text(scale, name_x, y, white, key_row_names[KEYROW_TOTAL], font);
	key_numbers(buf, sizeof(buf), k->total, k->columns);
	graph_text(scale, num_x, y, white, buf, font);
	y += row;
	graph_text(scale, name_x, y, GRAPH_CPU, key_row_names[KEYROW_CPU], font);
	key_numbers(buf, sizeof(buf), k->cpu, k->columns);
	graph_text(scale, num_x, y, GRAPH_CPU, buf, font);
	y += row;
	graph_text(scale, name_x, y, grey, key_row_names[KEYROW_FRAME], font);
	key_numbers(buf, sizeof(buf), k->frame, k->columns);
	graph_text(scale, num_x, y, grey, buf, font);
	/* What makes the GPU's total short of frame to frame (Matt 11-14: "the GPU total
	   numbers are not matching"): the queue's idle BEFORE the frame started, in no
	   GPU frame - a CPU stall ahead of R_BeginFrame is here, not in gpu idle. */
	if (k->gap[0] > 0.f || k->gap[1] > 0.f)
	{
		y += row;
		graph_text(scale, name_x, y, grey, key_row_names[KEYROW_GAP], font);
		key_numbers(buf, sizeof(buf), k->gap, k->columns);
		graph_text(scale, num_x, y, grey, buf, font);
	}
	if (sound_now > 0.0)
	{
		y += row;
		graph_text(scale, name_x, y, grey, key_row_names[KEYROW_SOUND], font);
		const float v[3] = { (float)sound_now, (float)avg_result_ms(PROFILER_SOUND_TRACE), 0.f };
		key_numbers(buf, sizeof(buf), v, 2);
		graph_text(scale, num_x, y, grey, buf, font);
	}

	R_ClearColor();
	R_SetScale(1.0f);
	return y + row;
}

/* The long lines, under BOTH keys (Matt, 11-14): the unaccounted gap was the GPU
   key's last row and the blood counts the CPU key's, and either ran across into the
   other column - the two printed over each other. */
static void
draw_key_footer(float scale, qhandle_t font, int x, int y, int row, const key_values_t *k)
{
	const uint32_t grey = MakeColor(170, 170, 170, 255);
	const profiler_frame_extras_t *extras = shown_extras();

	R_SetScale(scale);
	if (extras->gap_largest_ms >= 0.05f)
	{
		char gap[160], buf[192];
		gap_text(gap, sizeof(gap), extras);
		Q_snprintf(buf, sizeof(buf), "unaccounted: %s", gap);
		graph_text(scale, x, y, grey, buf, font);
		y += row;
	}
	char line1[160], line2[160];
	blood_count_text(line1, line2, sizeof(line1), k->counts);
	graph_text(scale, x, y, grey, line1, font);
	graph_text(scale, x, y + row, grey, line2, font);
	R_ClearColor();
	R_SetScale(1.0f);
}

// the GPU's idle between frames over the last n samples into the key
static void
key_gap(key_values_t *k, int n)
{
	int counted = 0;
	for (int i = 0; i < n; i++)
	{
		const int s = (graph.next - 1 - i + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		if (graph.shot[s])
			continue;
		const float g = graph.gap_ms[s];
		k->gap[1] += g;
		k->gap[2] = max(k->gap[2], g);
		counted++;
	}
	if (n > 0)
	{
		k->gap[1] /= (float)max(counted, 1);
		k->gap[0] = graph.gap_ms[(graph.next - 1 + GRAPH_SAMPLES) % GRAPH_SAMPLES];
	}
	if (cap.hold)
		k->gap[0] = cap.held_extras.gpu_gap_ms;
}

// the CPU frame by section (cpu_rows_of) over the last n samples into the key
static void
key_cpu_rows(key_values_t *k, int n)
{
	int counted = 0;
	for (int i = 0; i < n; i++)
	{
		const int s = (graph.next - 1 - i + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		if (graph.shot[s])
			continue;	// the capture's screenshot (graph.shot)
		const float *v = graph.cpu_rows[s];
		for (int r = 0; r < NUM_CPUROWS; r++)
		{
			k->cpu_rows[1][r] += v[r];
			k->cpu_rows[2][r] = max(k->cpu_rows[2][r], v[r]);
		}
		counted++;
	}
	if (n > 0)
	{
		const int latest = (graph.next - 1 + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		for (int r = 0; r < NUM_CPUROWS; r++)
			k->cpu_rows[1][r] /= (float)max(counted, 1);
		memcpy(k->cpu_rows[0], graph.cpu_rows[latest], sizeof(k->cpu_rows[0]));
		memcpy(k->counts, graph.counts[latest], sizeof(k->counts));
	}
	if (cap.hold)
	{
		memcpy(k->cpu_rows[0], cap.held_cpu_rows, sizeof(k->cpu_rows[0]));
		memcpy(k->counts, cap.held_counts, sizeof(k->counts));
	}
}

/* The CPU's key, beside the GPU's: its frame by section as a tree (a row shown
   when it reaches 0.05 ms in any column), the sum and the waits. What the blood
   work was is under both keys (draw_key_footer). Returns the first row under it. */
static int
draw_cpu_key(float scale, qhandle_t font, int key_x, int key_y, int row, const key_values_t *k)
{
	const float u = 1.f / scale;
	const uint32_t white = MakeColor(255, 255, 255, 255), grey = MakeColor(170, 170, 170, 255);
	const int num_x = key_x + (int)((float)cpu_key_num_units() * u);
	char buf[192];

	R_SetScale(scale);
	graph_text(scale, key_x, key_y - row, white, "cpu by section", font);
	Q_snprintf(buf, sizeof(buf), k->columns >= 3 ? "%s     avg     max" : "%s     avg", cap.hold ? "  held" : "   now");
	graph_text(scale, num_x, key_y - row, grey, buf, font);
	int y = key_y;
	for (int r = 0; r < NUM_CPUROWS; r++)
	{
		const int depth = cpurow_info[r].depth;
		const float v[3] = { k->cpu_rows[0][r], k->cpu_rows[1][r], k->cpu_rows[2][r] };
		const float most = max(v[0], max(v[1], k->columns >= 3 ? v[2] : 0.f));
		if (depth > 0 && most < 0.05f)
			continue;
		if (depth == 0 && r != CPUROW_VIEW && r != CPUROW_RENDER && most < 0.05f)
			continue;
		const uint32_t color = depth < 0 ? (r == CPUROW_TOTAL ? GRAPH_CPU : grey) : (depth == 0 ? white : grey);
		graph_text(scale, key_x + (int)((float)(6 * max(depth, 0)) * u), y, color, cpurow_info[r].name, font);
		key_numbers(buf, sizeof(buf), v, k->columns);
		graph_text(scale, num_x, y, color, buf, font);
		y += row;
	}
	R_ClearColor();
	R_SetScale(1.0f);
	return y;
}

/* profiler 5 and 6 (Matt, 11-14: "profilers 3 and 4 with only the graphs, colors and
   labels for the colors"): the GPU key without its numbers - each category's colour
   and name, top of the stack first as in the key, then the lines over the stack:
   the capture line and the CPU's frame time. */
static void
draw_legend(float scale, qhandle_t font, int x, int y, int row)
{
	const float u = 1.f / scale;
	const int thick = max(1, (int)(0.5f * u));
	const int sw = max(3, (int)(7.f * u)), mid = (int)(3.f * u);
	const int name_x = x + (int)((float)KEY_SWATCH_UNITS * u);
	const int lines_y = y + NUM_CATS * row + row / 2;
	const uint32_t white = MakeColor(255, 255, 255, 255);

	R_SetScale(1.0f);
	for (int j = 0; j < NUM_CATS; j++)
		draw_swatch(x, y + j * row + (int)u, u, NUM_CATS - 1 - j);
	R_DrawFill32(x, lines_y + mid, sw, thick, GRAPH_CAPTURE);
	R_DrawFill32(x, lines_y + row + mid, sw, thick, GRAPH_CPU);

	R_SetScale(scale);
	for (int j = 0; j < NUM_CATS; j++)
		graph_text(scale, name_x, y + j * row, white, cat_info[NUM_CATS - 1 - j].name, font);
	char buf[64];
	Q_snprintf(buf, sizeof(buf), "%.0f fps (%.2f ms)", capture_threshold_fps(), 1000.f / capture_threshold_fps());
	graph_text(scale, name_x, lines_y, white, buf, font);
	graph_text(scale, name_x, lines_y + row, GRAPH_CPU, key_row_names[KEYROW_CPU], font);
	R_ClearColor();
	R_SetScale(1.0f);
}

/* profiler 3 (Matt, 11-14): the GPU frame by category over the last few seconds, a
   stacked column a frame with the newest on the right, the capture line in white,
   the CPU's frame time over it in dark green (its own work, the waits taken out).
   Where the green line rises over the stack, the CPU is the limit; where "gpu idle"
   grows, the GPU was waiting for the CPU to finish the frame. The key gives each category this frame
   (the held one while a capture holds), its average and its worst over the panel.
   profiler 5: the panel alone, its legend to the right (keys false). */
static void
draw_cat_history(float scale, qhandle_t font, bool keys)
{
	const float u = 1.f / scale;
	const int top = 100;		// where the table starts, in pixels
	const int left = panel_left(u, keys ? max(352, keys_units(3)) : 352 + LEGEND_GAP + legend_units());
	const int w = (int)(352.f * u), h = (int)(128.f * u), row = (int)(10.f * u);
	const int thick = max(1, (int)(0.5f * u));
	const int cw = max(1, (int)(0.75f * u + 0.5f));	// 3 pixels a frame at 4K

	/* The 2D queue holds 16K quads a frame, the HUD's and the console's included,
	   and a column is up to NUM_CATS of them and the lines' two: as many columns as
	   what is left takes, with room kept for the text after. */
	int queued = 0, queue_max = 0;
	size_t record = 0;
	vkpt_draw_stretch_pic_queue(&queued, &record, &queue_max);
	const int room = max(0, queue_max - queued - 5000);	// the keys' text: ~3000 characters
	const int n = max(0, min(min(graph.count, w / cw), room / (NUM_CATS + 2)));

	static float cpu[GRAPH_SAMPLES];
	double span = 0.0;
	key_values_t k;
	memset(&k, 0, sizeof(k));
	k.columns = 3;
	int timed = 0, cpu_counted = 0;
	for (int i = 0; i < n; i++)
	{
		const int s = (graph.next - n + i + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		span += graph.frame_ms[s];
		// the capture's screenshot: a gap in the line, and in no key (graph.shot)
		cpu[i] = graph.shot[s] ? 0.f : graph.cpu_ms[s];
		if (!graph.shot[s])
		{
			k.cpu[1] += cpu[i];
			k.cpu[2] = max(k.cpu[2], cpu[i]);
			k.frame[1] += graph.frame_ms[s];
			k.frame[2] = max(k.frame[2], graph.frame_ms[s]);
			cpu_counted++;
		}
		if (graph.gpu_ms[s] > 0.f)
		{
			timed++;
			for (int c = 0; c < NUM_CATS; c++)
			{
				k.cat[1][c] += graph.cat_ms[s][c];
				k.cat[2][c] = max(k.cat[2][c], graph.cat_ms[s][c]);
			}
			const float t = cat_total(graph.cat_ms[s]);
			k.total[1] += t;
			k.total[2] = max(k.total[2], t);
		}
	}
	if (n > 0)
	{
		k.cpu[1] /= (float)max(cpu_counted, 1);
		k.frame[1] /= (float)max(cpu_counted, 1);
		k.cpu[0] = graph.cpu_ms[(graph.next - 1 + GRAPH_SAMPLES) % GRAPH_SAMPLES];
		k.frame[0] = graph.frame_ms[(graph.next - 1 + GRAPH_SAMPLES) % GRAPH_SAMPLES];
		memcpy(k.cat[0], graph.cat_ms[(graph.next - 1 + GRAPH_SAMPLES) % GRAPH_SAMPLES], sizeof(k.cat[0]));
	}
	if (timed)
	{
		for (int c = 0; c < NUM_CATS; c++)
			k.cat[1][c] /= (float)timed;
		k.total[1] /= (float)timed;
	}
	if (cap.hold)
	{
		memcpy(k.cat[0], cap.held_cat, sizeof(k.cat[0]));
		k.cpu[0] = cap.worst_cpu;
		k.frame[0] = cap.worst_frame;
	}
	k.total[0] = cat_total(k.cat[0]);
	key_cpu_rows(&k, n);
	key_gap(&k, n);

	const float vtop = max(graph.cat_top, GRAPH_FRAME_TOP);
	const int box = keys ? top + 3 * row : top;	// under the title's three lines
	const int key_y = box + h + 2 * row;	// the key's first row, under its header
	const int seg_gap = max(1, thick / 2);

	R_SetScale(1.0f);
	graph_panel(left, box, w, h);
	for (int i = 0; i < n; i++)
		if (graph.shot[(graph.next - n + i + GRAPH_SAMPLES) % GRAPH_SAMPLES])
			R_DrawFill32(left + w - (n - i) * cw, box, cw, h, MakeColor(255, 255, 255, 45));
	for (int i = 0; i < n; i++)
		draw_stack(left + w - (n - i) * cw, cw, box + h, h, vtop,
			graph.cat_ms[(graph.next - n + i + GRAPH_SAMPLES) % GRAPH_SAMPLES], seg_gap, 0, graph.next - n + i);
	draw_capture_line(left, box, w, h, thick, vtop);
	if (n > 0)
	{
		graph_line(left, box, w, h, thick, cw, cpu, n, vtop, GRAPH_CPU);
	}

	R_SetScale(scale);
	const uint32_t white = MakeColor(255, 255, 255, 255), grey = MakeColor(170, 170, 170, 255);
	char buf[160];
	if (keys)
	{
		/* the lines' meaning over two lines: as one (107 characters) it ran off a 4K
		   screen's right edge from 500 pixels in (Matt's 11-14 shot) */
		graph_text(scale, left, top, white, "gpu frame by category, newest on the right", font);
		Q_snprintf(buf, sizeof(buf), "the last %.1f s; white line %.0f fps (%.2f ms)",
			span * 1e-3, capture_threshold_fps(), 1000.f / capture_threshold_fps());
		graph_text(scale, left, top + row, grey, buf, font);
		graph_text(scale, left, top + 2 * row, grey, "dark green the cpu's own time; a faint column is a capture's screenshot", font);
	}

	// the panel's scale, at its top and middle lines, to its right
	const int label_x = left + w + (int)(4.f * u);
	Q_snprintf(buf, sizeof(buf), "%.1f ms", vtop);
	graph_text(scale, label_x, box, grey, buf, font);
	Q_snprintf(buf, sizeof(buf), "%.1f", vtop * 0.5f);
	graph_text(scale, label_x, box + h / 2 - (int)(4.f * u), grey, buf, font);
	R_ClearColor();

	if (!keys)
	{
		draw_legend(scale, font, left + w + (int)((float)LEGEND_GAP * u), box, row);
		return;
	}
	const int gpu_end = draw_key(scale, font, left, key_y, row, &k);
	const int cpu_end = draw_cpu_key(scale, font, left + (int)((float)cpu_key_offset_units(3) * u), key_y, row, &k);
	draw_key_footer(scale, font, left, max(gpu_end, cpu_end) + row / 2, row, &k);
}

/* profiler 4 (Matt, 11-14): two stacked bars - this frame's GPU frame by category
   (the held one while a capture holds) and the average over profiler_samples
   frames - each with the CPU's frame time on it as a dark green tick, the capture
   line across both, and the key to their right with both numbers.
   profiler 6: the bars alone, their legend to the right (keys false). */
static void
draw_cat_bars(float scale, qhandle_t font, bool keys)
{
	const float u = 1.f / scale;
	const int top = 100;
	const int w_units = 3 * 16 + 2 * 44;	// the panel: the gaps and the bars below
	const int left = panel_left(u, w_units + (keys ? 48 + keys_units(2) : LEGEND_GAP + legend_units()));
	const int h = (int)(128.f * u), row = (int)(10.f * u);
	const int thick = max(1, (int)(0.5f * u));
	const int bar = (int)(44.f * u), gap = (int)(16.f * u);
	const int w = 3 * gap + 2 * bar;
	const int box = keys ? top + 2 * row : top, bottom = box + h;
	const int now_x = left + gap, avg_x = left + 2 * gap + bar;

	key_values_t k;
	memset(&k, 0, sizeof(k));
	k.columns = 2;
	const int want = max(1, cvar_profiler_samples ? cvar_profiler_samples->integer : 60);
	const int n = min(graph.count, min(want, GRAPH_SAMPLES));
	int timed = 0, cpu_counted = 0;
	for (int i = 0; i < n; i++)
	{
		const int s = (graph.next - 1 - i + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		if (!graph.shot[s])
		{
			k.cpu[1] += graph.cpu_ms[s];
			k.frame[1] += graph.frame_ms[s];
			cpu_counted++;
		}
		if (graph.gpu_ms[s] > 0.f)
		{
			timed++;
			for (int c = 0; c < NUM_CATS; c++)
				k.cat[1][c] += graph.cat_ms[s][c];
		}
	}
	if (n > 0)
	{
		const int s = (graph.next - 1 + GRAPH_SAMPLES) % GRAPH_SAMPLES;
		k.cpu[1] /= (float)max(cpu_counted, 1);
		k.frame[1] /= (float)max(cpu_counted, 1);
		k.cpu[0] = graph.cpu_ms[s];
		k.frame[0] = graph.frame_ms[s];
		memcpy(k.cat[0], graph.cat_ms[s], sizeof(k.cat[0]));
	}
	if (timed)
		for (int c = 0; c < NUM_CATS; c++)
			k.cat[1][c] /= (float)timed;
	if (cap.hold)
	{
		memcpy(k.cat[0], cap.held_cat, sizeof(k.cat[0]));
		k.cpu[0] = cap.worst_cpu;
		k.frame[0] = cap.worst_frame;
	}
	k.total[0] = cat_total(k.cat[0]);
	k.total[1] = cat_total(k.cat[1]);
	key_cpu_rows(&k, n);
	key_gap(&k, n);

	const float vtop = max(graph.cat_top, GRAPH_FRAME_TOP);
	const int key_x = left + w + (int)(48.f * u), key_y = box + row;

	R_SetScale(1.0f);
	graph_panel(left, box, w, h);
	const int stripe = max(1, (int)(0.75f * u));
	draw_stack(now_x, bar, bottom, h, vtop, k.cat[0], thick, stripe, 0);
	draw_stack(avg_x, bar, bottom, h, vtop, k.cat[1], thick, stripe, 0);
	draw_capture_line(left, box, w, h, thick, vtop);
	// the CPU's frame time (dark green) on each bar, a little wider than it
	const int over = (int)(4.f * u);
	for (int b = 0; b < 2; b++)
		if (k.cpu[b] > 0.f)
			R_DrawFill32((b ? avg_x : now_x) - over,
				bottom - (int)(min(k.cpu[b] / vtop, 1.f) * (float)(h - thick)) - thick,
				bar + 2 * over, thick, GRAPH_CPU);

	R_SetScale(scale);
	const uint32_t white = MakeColor(255, 255, 255, 255), grey = MakeColor(170, 170, 170, 255);
	char buf[160];
	if (keys)
	{
		graph_text(scale, left, top, white, "gpu frame by category", font);
		Q_snprintf(buf, sizeof(buf), "%s and the average of the last %d; white line %.0f fps (%.2f ms)",
			cap.hold ? "the held frame" : "this frame", n, capture_threshold_fps(), 1000.f / capture_threshold_fps());
		graph_text(scale, left, top + row, grey, buf, font);
	}

	// under the bars, and the panel's scale to its right
	graph_text(scale, now_x + bar / 2 - (int)(12.f * u), bottom + row / 4, grey, cap.hold ? "held" : " now", font);
	graph_text(scale, avg_x + bar / 2 - (int)(12.f * u), bottom + row / 4, grey, " avg", font);
	const int label_x = left + w + (int)(4.f * u);
	Q_snprintf(buf, sizeof(buf), "%.1f ms", vtop);
	graph_text(scale, label_x, box, grey, buf, font);
	Q_snprintf(buf, sizeof(buf), "%.1f", vtop * 0.5f);
	graph_text(scale, label_x, box + h / 2 - (int)(4.f * u), grey, buf, font);
	R_ClearColor();

	if (!keys)
	{
		draw_legend(scale, font, left + w + (int)((float)LEGEND_GAP * u), box, row);
		return;
	}
	const int gpu_end = draw_key(scale, font, key_x, key_y, row, &k);
	const int cpu_end = draw_cpu_key(scale, font, key_x + (int)((float)cpu_key_offset_units(2) * u), key_y, row, &k);
	draw_key_footer(scale, font, key_x, max(gpu_end, cpu_end) + row / 2, row, &k);
}

void
draw_profiler(int enable_asvgf, const char *title)
{
	float profiler_scale = R_ClampScale(cvar_profiler_scale);
	const int mode = cvar_profiler ? cvar_profiler->integer : 1;

	// 5 and 6 are 3 and 4 without the numbers: the graph and its legend
	if (mode >= 2 && mode <= 6)
	{
		const qhandle_t font = R_RegisterFont("conchars");
		if (!font)
			return;
		if (mode == 2)
			draw_graphs(profiler_scale, font);
		else if (mode == 3 || mode == 5)
			draw_cat_history(profiler_scale, font, mode == 3);
		else
			draw_cat_bars(profiler_scale, font, mode == 4);
		draw_capture_text(profiler_scale, font);
		return;
	}

	int x = 500 * profiler_scale;
	// The title line 100 pixels down at every scale, the headers 14 units under
	// it and the rows 16 under those - room kept for the title with or without
	// one. 11-07: D3D12's pt_ser line was drawn 30 units above y = 100 pixels,
	// which at 4K (scale 0.25: 25 units) put it off the top of the screen.
	int y = 100 * profiler_scale + 30;

	qhandle_t font;
	font = R_RegisterFont("conchars");
	if(!font)
		return;

	R_SetScale(profiler_scale);

	if (title)
		R_DrawString(x, y - 30, 0, 128, title, font);

	// Table headers
	R_DrawString(x + 256, y - 16, 0, 128, "    imm         avg", font);

	// The whole frame first - what the fps counter shows when the GPU is the
	// limit (11-03: Matt read 67 fps against a "frame time" of 13.5 ms; Flow's
	// simulation, motion blur, bloom, tone mapping, the blit and the HUD all
	// run outside that). Then the old frame time, which DRS and frame
	// generation pace from: up to the END of DLSS, DLSS included (both
	// renderers stop it after the DLSS pass). It read "(to dlss)" until 11-07,
	// and Matt added the DLSS row to it: 9.7 + 3.6 ms in an 11 ms frame.
	draw_query(x, y, font, "GPU_FRAME_(WHOLE)", PROFILER_GPU_FRAME); y += 10;
	draw_query(x, y, font, "FRAME_TIME_(THROUGH_DLSS)", PROFILER_FRAME_TIME); y += 10;

	// the passes this frame shows, collected, then drawn in the order they ran
	profiler_row_t rows[NUM_PROFILER_ENTRIES];
	int n = 0;
#define PROFILER_DO(entry, indent) \
	do { if (n < NUM_PROFILER_ENTRIES) { rows[n].idx = entry; rows[n].listed = n; rows[n].name = &#entry[9]; n++; } } while (0)

	PROFILER_DO(PROFILER_UPLOAD_LIGHTS, 1);
	PROFILER_DO(PROFILER_INSTANCE_GEOMETRY, 1);
	PROFILER_DO(PROFILER_BVH_UPDATE, 1);	
	PROFILER_DO(PROFILER_UPDATE_ENVIRONMENT, 1);
	PROFILER_DO(PROFILER_SHADOW_MAP, 1);
	PROFILER_DO(PROFILER_PRIMARY_RAYS, 1);
	if (cvar_pt_reflect_refract->integer > 0) { PROFILER_DO(PROFILER_REFLECT_REFRACT_1, 1); }
	if (cvar_pt_reflect_refract->integer > 1) { PROFILER_DO(PROFILER_REFLECT_REFRACT_2, 1); }
	if (enable_asvgf)
	{
		PROFILER_DO(PROFILER_ASVGF_GRADIENT_REPROJECT, 1);
	}
	if (cvar_pt_sharc->value != 0) { PROFILER_DO(PROFILER_SHARC, 1); }
	PROFILER_DO(PROFILER_DIRECT_LIGHTING, 1);
	PROFILER_DO(PROFILER_INDIRECT_LIGHTING, 1);
	PROFILER_DO(PROFILER_INDIRECT_LIGHTING_0, 2);
	PROFILER_DO(PROFILER_INDIRECT_LIGHTING_1, 2);
	if (cvar_pt_restir_gi->value != 0) { PROFILER_DO(PROFILER_RESTIR_GI, 1); }
	PROFILER_DO(PROFILER_GOD_RAYS, 1);
	PROFILER_DO(PROFILER_GOD_RAYS_REFLECT_REFRACT, 1);
	PROFILER_DO(PROFILER_GOD_RAYS_FILTER, 1);
	if (cvar_pt_fog_froxel && cvar_pt_fog_froxel->integer)
	{
		PROFILER_DO(PROFILER_FOG_FROXEL, 1);
		PROFILER_DO(PROFILER_FOG_FROXEL_RESERVOIR, 2);
		PROFILER_DO(PROFILER_FOG_FROXEL_SPATIAL, 2);
		PROFILER_DO(PROFILER_FOG_FROXEL_SCATTER, 2);
		PROFILER_DO(PROFILER_FOG_FROXEL_INTEGRATE, 2);
	}
	// All three whenever Flow is there at all, N/A on a frame one did not run:
	// listed only while it ran, the march's row came and went with the beam and
	// the table jumped by a row (Matt, 11-12). The march has a row of its own
	// only when it ran as its own pass (vkpt_pt_flow_split_ran); otherwise it is
	// inside primary rays.
	if (vkpt_flow_profiled())
	{
		PROFILER_DO(PROFILER_FLOW_SIMULATION, 1);
		PROFILER_DO(PROFILER_FLOW_SMOKE_LIGHTING, 1);
		PROFILER_DO(PROFILER_FLOW_MARCH, 1);
	}
	if (enable_asvgf)
	{
		PROFILER_DO(PROFILER_ASVGF_FULL, 1);
		PROFILER_DO(PROFILER_ASVGF_RECONSTRUCT_GRADIENT, 2);
		PROFILER_DO(PROFILER_ASVGF_TEMPORAL, 2);
		PROFILER_DO(PROFILER_ASVGF_ATROUS, 2);
		PROFILER_DO(PROFILER_ASVGF_TAA, 2);
	}
	else
	{
		PROFILER_DO(PROFILER_COMPOSITING, 1);
		// photo mode's running average, only while it runs
		if (shown_results()[PROFILER_ACCUMULATION * 2 + 1])
			PROFILER_DO(PROFILER_ACCUMULATION, 1);
	}
	if (qvk.device_count > 1) {
		PROFILER_DO(PROFILER_MGPU_TRANSFERS, 1);
	}
	PROFILER_DO(PROFILER_INTERLEAVE, 1);
	// Only listed while it is actually running - an unconditional row would
	// read 0.00 for everyone who has motion blur off, which is most people.
	if (vkpt_motion_blur_is_enabled())
	{
		PROFILER_DO(PROFILER_MOTION_BLUR, 1);
	}
	if (vkpt_chromatic_aberration_is_enabled())
	{
		PROFILER_DO(PROFILER_CHROMATIC_ABERRATION, 1);
	}
	PROFILER_DO(PROFILER_BLOOM, 1);
	PROFILER_DO(PROFILER_TONE_MAPPING, 2);
	if (DLSSEnabled())
	{
		PROFILER_DO(PROFILER_DLSS, 1);
	}
	/* What used to be "other" (profiler.h): timed on D3D12 only, so listed there;
	   DLSS 5 and the sound only while they run. */
	if (profiler_data.d3d12)
	{
		const uint64_t *rr = shown_results();
		if (rr[PROFILER_DLSS5 * 2 + 1])
			PROFILER_DO(PROFILER_DLSS5, 1);
		PROFILER_DO(PROFILER_TEXTURE_UPLOAD, 1);
		PROFILER_DO(PROFILER_BLOOD_UPLOAD, 1);
		PROFILER_DO(PROFILER_TRANSPARENT_UPLOAD, 1);
		PROFILER_DO(PROFILER_UPLOAD_QUEUE, 1);
		PROFILER_DO(PROFILER_HUD_PRESENT, 1);
		PROFILER_DO(PROFILER_GPU_IDLE, 1);
		if (rr[PROFILER_SOUND_TRACE * 2 + 1])
			PROFILER_DO(PROFILER_SOUND_TRACE, 1);
	}
#undef PROFILER_DO

	note_row_times();
	order_rows(rows, n);
	for (int i = 0; i < n; i++)
	{
		draw_query(x, y, font, rows[i].name, rows[i].idx);
		y += 10;
	}

	R_SetScale(1.0f);
	draw_capture_text(profiler_scale, font);
}

double vkpt_get_profiler_result(int idx)
{
	uint64_t begin = profiler_data.query_pool_results[idx * 2 + 0];
	uint64_t end = profiler_data.query_pool_results[idx * 2 + 1];

	if (begin == 0 || end == 0)
		return 0.0; // one of these queries was unavailable at the time vkGetQueryPoolResults was called

	double ms = (double)(end - begin) * 1e-6 * qvk.timestampPeriod;
	return ms;
}

/* profiler_dump: print every pass's running average (over profiler_samples frames) to
   the console, so a scripted benchmark can read per-pass GPU cost from the log instead
   of from the overlay. Passes that did not run this frame are skipped. */
void
vkpt_profiler_dump(void)
{
	const char *names[] = {
#define PROFILER_DO(name, indent) #name,
		PROFILER_LIST
#undef PROFILER_DO
	};

	Com_Printf("PROFDUMP begin api=%s samples=%d\n",
		profiler_data.d3d12 ? "d3d12" : qvk.use_ray_query ? "query" : "pipeline", (int)profiler_data.allocated_samples);
	for (int idx = 0; idx < NUM_PROFILER_ENTRIES; idx++)
	{
		const profiler_entry_samples_t *e = &profiler_data.samples[idx];
		if (e->num_samples == 0)
			continue;
		double avg_ms = ((double)e->accumulated / (e->num_samples * 1e6)) * qvk.timestampPeriod;
		Com_Printf("PROFDUMP %-34s %8.3f ms\n", names[idx] + 9, avg_ms);
	}

	// counted since the previous dump
	const int n = upload_spikes.frames;
	Com_Printf("UPLOADSPIKES transfer_on_graphics=%d frames=%d upload_mean=%.3f upload_max=%.2f "
		"up>2=%d up>5=%d up>20=%d cpu_max=%.2f cpu>25=%d cpu>50=%d both=%d\n",
		profiler_data.d3d12 || qvk.queue_idx_transfer == qvk.queue_idx_graphics, n,
		n ? upload_spikes.upload_sum / n : 0.0, upload_spikes.upload_max,
		upload_spikes.upload_over_2, upload_spikes.upload_over_5, upload_spikes.upload_over_20,
		upload_spikes.cpu_max, upload_spikes.cpu_over_25, upload_spikes.cpu_over_50, upload_spikes.both);
	for (int i = 0; i < SPIKE_WORST && upload_spikes.worst[i].upload_ms > 0.f; i++)
		Com_Printf("UPLOADSPIKE upload=%.2f gpu_frame=%.2f cpu_window=%.2f\n",
			upload_spikes.worst[i].upload_ms, upload_spikes.worst[i].gpu_frame_ms, upload_spikes.worst[i].cpu_ms);
	upload_spikes_reset();

	Com_Printf("PROFDUMP end\n");
}
