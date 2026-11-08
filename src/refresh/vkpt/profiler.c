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
#include "system/system.h"

#include <assert.h>


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

static struct {
	uint64_t last_us;
	float frame_ms[GRAPH_SAMPLES];
	float gpu_ms[GRAPH_SAMPLES];	// 0 where the GPU frame was not timed
	uint8_t presents[GRAPH_SAMPLES];	// DLSS-G's presents per rendered frame, 1 without
	int next, count;
	// the panels' tops (graph_scale)
	float frame_top, fps_top;
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

static void
graph_record(void)
{
	const uint64_t now = Sys_Microseconds();
	const float ms = graph.last_us ? (float)(now - graph.last_us) * 1e-3f : 0.f;
	graph.last_us = now;
	if (ms <= 0.f)
		return;
	// Two seconds or more is a level load, a vid_restart or the window put away:
	// start over, rather than carry it in the averages for the next ten seconds.
	if (ms >= 2000.f)
	{
		graph.count = graph.next = 0;
		graph.frame_top = graph.fps_top = 0.f;
		graph.text_ms = 0;
		return;
	}
	// what the renderer last reported (the fps counter's "presented")
	const int presents = vkpt_refdef.fd ? vkpt_refdef.fd->feedback.presented_frames : 1;
	graph.frame_ms[graph.next] = ms;
	graph.gpu_ms[graph.next] = (float)vkpt_get_profiler_result(PROFILER_GPU_FRAME);
	graph.presents[graph.next] = (uint8_t)max(1, min(presents, 255));
	graph.next = (graph.next + 1) % GRAPH_SAMPLES;
	graph.count = min(graph.count + 1, GRAPH_SAMPLES);
	graph_scale(&graph.frame_top, GRAPH_FRAME_TOP, graph_frame_value);
	graph_scale(&graph.fps_top, GRAPH_FPS_TOP, graph_fps_value);
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

	graph_record();
}

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

	graph_record();

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
	double ms = vkpt_get_profiler_result(idx);
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

static void note_row_times(void)
{
	const uint64_t *r = profiler_data.query_pool_results;
	const uint64_t frame = r[PROFILER_GPU_FRAME * 2] ? r[PROFILER_GPU_FRAME * 2] : r[PROFILER_FRAME_TIME * 2];
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

// The graphs' colours: frame time amber, the GPU's green, fps blue, frame
// generation's presented fps violet
#define GRAPH_FRAME	MakeColor(255, 200, 60, 255)
#define GRAPH_GPU	MakeColor(90, 220, 90, 255)
#define GRAPH_FPS	MakeColor(90, 170, 255, 255)
#define GRAPH_FG	MakeColor(225, 120, 255, 255)

static int
graph_cmp(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

/* A series as a line, a sample a pixel column with the newest at the right edge,
   each column joined to the one before. A value over the top runs along it; 0 (not
   measured) is a gap. */
static void
graph_line(int left, int top, int w, int h, int thick, const float *v, int n, float vmax,
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
		R_DrawFill32(left + w - n + i, y0, 1, y1 - y0 + thick, color);
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
		graph_line(left, frame_box, w, h, thick, gpu, n, graph.frame_top, GRAPH_GPU);
		graph_line(left, frame_box, w, h, thick, frame, n, graph.frame_top, GRAPH_FRAME);
		if (fg_n)
			graph_line(left, fps_box, w, h, thick, fg, n, graph.fps_top, GRAPH_FG);
		graph_line(left, fps_box, w, h, thick, fps, n, graph.fps_top, GRAPH_FPS);
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

void
draw_profiler(int enable_asvgf, const char *title)
{
	float profiler_scale = R_ClampScale(cvar_profiler_scale);

	if (cvar_profiler && cvar_profiler->integer == 2)
	{
		const qhandle_t font = R_RegisterFont("conchars");
		if (font)
			draw_graphs(profiler_scale, font);
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
	if (vkpt_flow_profiled())
	{
		PROFILER_DO(PROFILER_FLOW_SIMULATION, 1);
		if (vkpt_flow_env_profiled()) { PROFILER_DO(PROFILER_FLOW_SMOKE_LIGHTING, 1); }
		if (vkpt_pt_flow_split_ran()) { PROFILER_DO(PROFILER_FLOW_MARCH, 1); }
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
#undef PROFILER_DO

	note_row_times();
	for (int i = 1; i < n; i++)	// insertion sort: a few dozen rows, nearly in order already
	{
		const profiler_row_t r = rows[i];
		int j = i;
		for (; j > 0 && row_before(&r, &rows[j - 1]); j--)
			rows[j] = rows[j - 1];
		rows[j] = r;
	}
	for (int i = 0; i < n; i++)
	{
		draw_query(x, y, font, rows[i].name, rows[i].idx);
		y += 10;
	}

	R_SetScale(1.0f);
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
