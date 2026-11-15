/*
THE PROFILER (profiler 1, profiler_dump) for the D3D12 renderer.

The same rows as vkpt's (vkpt/profiler.h), timed where vkpt times them: a
timestamp pair per entry, at entry * 2 + PROFILER_START/STOP of the frame slot's
range in one query heap, written into the frame's list by passes.c and world.c
(d3d12_timer_mark), resolved at the list's end - only the queries written, in
runs - and read back when the slot comes round again, after R_BeginFrame's fence
wait. The ticks go to vkpt's profiler.c (vkpt_profiler_take_results), which keeps
the averages, draws the table (draw_profiler) and prints profiler_dump, so both
renderers' numbers are the same arithmetic over the same spans.

As on Vulkan, the queries are written every frame and `profiler` only draws them.

Where the spans differ from vkpt's:
- Two stamps go on the queue in command lists of their own, as vkpt's
  vkpt_profiler_query_after does: GPU_FRAME's start, in R_BeginFrame - ahead of
  Flow's simulation, which executes on the queue during R_RenderFrame, before
  the frame's list - and FLOW_SIMULATION's pair around Flow's work (flow.c's
  flow_profile, through vkpt_flow_backend->profile).
- Three of vkpt's spans are two pieces here, and the second piece is timed apart
  and added to the first (passes.h): the sun's shadow map (the static half at
  the head of the list, the animated models after their BLAS builds), the BVH
  update (the dynamic BLASes, then the TLASes after the shadow map's second
  half), and Flow's occupancy and light sums, which vkpt submits right after
  Flow's work but which are in the frame's list here.
- GPU_FRAME ends with the frame's list: the HUD and the copy (or frame
  generation's sources) included, DLSS-G's present queue work not.

WHAT "OTHER" IS MADE OF (profiler 3/4, Matt 11-14: dips where it went from 1 ms to
8 and 25 with every pass flat). GPU_FRAME starts on the queue at R_BeginFrame, but
the frame's list is only executed at R_EndFrame, so the span holds:
- GPU_IDLE: from that start to the list's first command (its STOP, marked first
  thing in the list) less the work the queue had in between - Flow's simulation
  and the upload lists (UPLOAD_QUEUE, up to four, d3d12_upload_flush) - is the GPU
  with nothing to do: the CPU still recording the frame. Its START is made up here
  (collect) so the row's length is that. d3d12_sound_queue 1 runs would be in it.
- in the list: the rows marked there, and between them what no row covers -
  barriers, clears, anything not yet timed. collect measures that (the union of
  the rows' spans against the list's) and names the longest stretch by the rows
  either side of it, so an untimed piece of work shows where it is.
- the CPU's own side of the same frame: R_BeginFrame (after the fence) to the
  list's submit, kept per slot and handed over with the slot's results - the
  number GPU_IDLE should be compared with, since profiler.c's frame to frame
  time is of a later frame (the results are D3D12_FRAMES_IN_FLIGHT behind).
- SOUND_TRACE is not in the frame: the last ray traced sound run's GPU time, on
  its own queue beside it (sound_trace.c), shown while runs are recent.
*/

#include "d3d12pt.h"
#include "passes.h"
#include "system/system.h"   // Sys_Microseconds
#include "common/cpuprof.h"   // cpuprof_last.serial: which CPU frame a slot's results are

// passes.h's second pieces, in order: the entry each is added to
static const int extra_target[] = {
    PROFILER_SHADOW_MAP,            // D3D12_PROFILER_SHADOW_MAP_DYNAMIC
    PROFILER_BVH_UPDATE,            // D3D12_PROFILER_BVH_TLAS
    PROFILER_FLOW_SIMULATION,       // D3D12_PROFILER_FLOW_PASSES
    PROFILER_UPLOAD_QUEUE,          // D3D12_PROFILER_UPLOAD_QUEUE_2
    PROFILER_UPLOAD_QUEUE,          // D3D12_PROFILER_UPLOAD_QUEUE_3
    PROFILER_UPLOAD_QUEUE,          // D3D12_PROFILER_UPLOAD_QUEUE_4
};
typedef char extra_targets_listed[sizeof(extra_target) / sizeof(extra_target[0]) ==
                                  D3D12_NUM_PROFILER_ENTRIES - NUM_PROFILER_ENTRIES ? 1 : -1];

static struct {
    cvar_t                      *profiler, *ser;
    ID3D12QueryHeap             *heap;
    ID3D12Resource              *readback;
    // the stamps outside the frame's list: an allocator per frame slot, reset
    // after the slot's fence, and one list recorded and executed a stamp at a time
    ID3D12CommandAllocator      *allocator[D3D12_FRAMES_IN_FLIGHT];
    ID3D12GraphicsCommandList   *list;
    double                      ns_per_tick;
    bool                        failed;
    bool                        resolved[D3D12_FRAMES_IN_FLIGHT];   // frame_end resolved the slot
    bool                        used[D3D12_FRAMES_IN_FLIGHT][D3D12_PROFILER_QUERIES];
    // the CPU's side of each slot's frame (Sys_Microseconds): R_BeginFrame, the submit
    uint64_t                    begin_us[D3D12_FRAMES_IN_FLIGHT], submit_us[D3D12_FRAMES_IN_FLIGHT];
    // the CPU frame each slot's frame closes as (cpuprof.h's CPUPROF_RING), handed
    // over with its results so the profiler shows that frame's CPU side with them
    uint32_t                    serial[D3D12_FRAMES_IN_FLIGHT];
    int                         uploads;    // upload lists timed this frame (UPLOAD_QUEUE and its extras)
} t;

static void release(void)
{
    D3D12_RELEASE(t.list);
    for (int i = 0; i < D3D12_FRAMES_IN_FLIGHT; i++)
        D3D12_RELEASE(t.allocator[i]);
    D3D12_RELEASE(t.heap);
    D3D12_RELEASE(t.readback);
}

static bool timers_on(void)
{
    if (t.heap)
        return true;
    if (t.failed)
        return false;

    t.failed = true;    // until made
    uint64_t frequency = 0;
    const D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                       D3D12_FRAMES_IN_FLIGHT * D3D12_PROFILER_QUERIES, 0 };
    bool ok = SUCCEEDED(ID3D12Device5_CreateQueryHeap(d3d.device, &qd, &IID_ID3D12QueryHeap, (void **)&t.heap));
    if (ok) {
        t.readback = d3d12_create_buffer(D3D12_FRAMES_IN_FLIGHT * D3D12_PROFILER_QUERIES * sizeof(uint64_t),
                                         D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, "profiler readback");
        ok = t.readback && SUCCEEDED(ID3D12CommandQueue_GetTimestampFrequency(d3d.queue, &frequency)) && frequency;
    }
    for (int i = 0; ok && i < D3D12_FRAMES_IN_FLIGHT; i++)
        ok = SUCCEEDED(ID3D12Device5_CreateCommandAllocator(d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            &IID_ID3D12CommandAllocator, (void **)&t.allocator[i]));
    // created closed: each stamp resets it
    if (ok)
        ok = SUCCEEDED(ID3D12Device5_CreateCommandList1(d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        D3D12_COMMAND_LIST_FLAG_NONE, &IID_ID3D12GraphicsCommandList,
                                                        (void **)&t.list));
    if (!ok) {
        release();
        Com_EPrintf("D3D12: no timestamp queries - the profiler shows nothing\n");
        return false;
    }
    d3d12_set_name((ID3D12Object *)t.list, "profiler stamps");
    t.ns_per_tick = 1e9 / (double)frequency;
    t.failed = false;
    memset(t.resolved, 0, sizeof(t.resolved));
    memset(t.used, 0, sizeof(t.used));
    return true;
}

/* One stamp on the queue in a list of its own (vkpt_profiler_query_after): written
   once everything executed on the queue before it has finished. */
static void stamp_on_queue(int entry, int action)
{
    const int slot = d3d.frame_index;
    if (!t.heap || !d3d.frame_open)
        return;
    if (FAILED(ID3D12GraphicsCommandList_Reset(t.list, t.allocator[slot], NULL)))
        return;
    const uint32_t q = (uint32_t)(entry * 2 + action);
    const UINT index = (UINT)(slot * D3D12_PROFILER_QUERIES) + q;
    ID3D12GraphicsCommandList_EndQuery(t.list, t.heap, D3D12_QUERY_TYPE_TIMESTAMP, index);
    /* resolved here too: the upload lists' stamps (d3d12_timers_upload_flush) come
       after frame_end has recorded the frame's resolve, which only takes the queries
       written by then - unresolved, collect would read an older frame's value */
    ID3D12GraphicsCommandList_ResolveQueryData(t.list, t.heap, D3D12_QUERY_TYPE_TIMESTAMP, index, 1, t.readback,
                                               (UINT64)index * sizeof(uint64_t));
    if (FAILED(ID3D12GraphicsCommandList_Close(t.list)))
        return;
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)t.list };
    ID3D12CommandQueue_ExecuteCommandLists(d3d.queue, 1, lists);
    t.used[slot][q] = true;
}

static uint64_t span_in(const uint64_t *ticks, int entry, uint64_t lo, uint64_t hi)
{
    uint64_t b = ticks[entry * 2], e = ticks[entry * 2 + 1];
    if (!b || !e || e <= b)
        return 0;
    b = max(b, lo);
    e = min(e, hi);
    return e > b ? e - b : 0;
}

typedef struct { uint64_t b, e; int entry; } span_t;

static int span_cmp(const void *x, const void *y)
{
    const span_t *a = x, *b = y;
    return (a->b > b->b) - (a->b < b->b);
}

// a row's name for the gap line: the second pieces by the row they are added to
static int named_entry(int entry)
{
    return entry >= NUM_PROFILER_ENTRIES ? extra_target[entry - NUM_PROFILER_ENTRIES] : entry;
}

/* The frame's raw stamps (before the second pieces are added) into GPU_IDLE and
   the gap figures - what "other" is made of (the comment at the top). */
static void account(uint64_t *ticks, bool *used, int slot, double ns_per_tick)
{
    profiler_frame_extras_t x = { .gap_after = -1, .gap_before = -1 };
    const double ms_per_tick = ns_per_tick * 1e-6;

    if (t.submit_us[slot] > t.begin_us[slot] && t.begin_us[slot])
        x.cpu_submit_ms = (float)((double)(t.submit_us[slot] - t.begin_us[slot]) * 1e-3);
    x.cpu_serial = t.serial[slot];

    const uint64_t frame_start = ticks[PROFILER_GPU_FRAME * 2], frame_end = ticks[PROFILER_GPU_FRAME * 2 + 1];

    /* The queue's time BETWEEN frames (11-14): GPU_FRAME runs from the frame's first
       stamp to its last, so a CPU stall before R_BeginFrame - Matt's captures had
       one of 88 ms with a GPU frame of 18.8 - is in no frame. Measured to the frame
       before, when it is the one before (the slots are collected in order). */
    static uint64_t prev_end;
    static uint32_t prev_serial;
    if (frame_start && frame_end >= frame_start) {
        if (prev_end && frame_start > prev_end && t.serial[slot] && t.serial[slot] == prev_serial + 1)
            x.gpu_gap_ms = (float)((double)(frame_start - prev_end) * ms_per_tick);
        prev_end = frame_end;
        prev_serial = t.serial[slot];
    }
    const uint64_t list_start = ticks[PROFILER_GPU_IDLE * 2 + 1];
    if (frame_start && list_start >= frame_start) {
        // the queue's own work between them: Flow's simulation and the upload lists
        uint64_t busy = span_in(ticks, PROFILER_FLOW_SIMULATION, frame_start, list_start) +
                        span_in(ticks, PROFILER_UPLOAD_QUEUE, frame_start, list_start);
        for (int e = D3D12_PROFILER_UPLOAD_QUEUE_2; e <= D3D12_PROFILER_UPLOAD_QUEUE_4; e++)
            busy += span_in(ticks, e, frame_start, list_start);
        const uint64_t window = list_start - frame_start;
        const uint64_t idle = window > busy ? window - busy : 0;
        // the row's length is the idle time; it ends where the list starts
        ticks[PROFILER_GPU_IDLE * 2] = list_start - idle;
        used[PROFILER_GPU_IDLE * 2] = true;
        if (!idle) {
            // a zero-length row reads as not run; keep it a tick long
            ticks[PROFILER_GPU_IDLE * 2] = list_start - 1;
        }
    } else {
        ticks[PROFILER_GPU_IDLE * 2] = ticks[PROFILER_GPU_IDLE * 2 + 1] = 0;
        used[PROFILER_GPU_IDLE * 2] = used[PROFILER_GPU_IDLE * 2 + 1] = false;
    }

    // the list: what the rows marked in it do not cover
    if (list_start && frame_end > list_start) {
        span_t spans[D3D12_NUM_PROFILER_ENTRIES];
        int n = 0;
        for (int e = 0; e < D3D12_NUM_PROFILER_ENTRIES; e++) {
            // the whole-frame spans, the made-up ones and the stamps on the queue are not the list's
            if (e == PROFILER_GPU_FRAME || e == PROFILER_FRAME_TIME || e == PROFILER_GPU_IDLE ||
                e == PROFILER_SOUND_TRACE || e == PROFILER_FLOW_SIMULATION || e == PROFILER_UPLOAD_QUEUE ||
                (e >= D3D12_PROFILER_UPLOAD_QUEUE_2 && e <= D3D12_PROFILER_UPLOAD_QUEUE_4))
                continue;
            uint64_t b = ticks[e * 2], en = ticks[e * 2 + 1];
            if (!used[e * 2] || !used[e * 2 + 1] || !b || en <= b)
                continue;
            b = max(b, list_start);
            en = min(en, frame_end);
            if (en > b)
                spans[n++] = (span_t){ b, en, e };
        }
        qsort(spans, n, sizeof(spans[0]), span_cmp);
        uint64_t covered_to = list_start, gap = 0, largest = 0;
        int after = -1;
        for (int i = 0; i < n; i++) {
            if (spans[i].b > covered_to) {
                const uint64_t g = spans[i].b - covered_to;
                gap += g;
                if (g > largest) {
                    largest = g;
                    x.gap_after = after;
                    x.gap_before = named_entry(spans[i].entry);
                }
            }
            if (spans[i].e > covered_to) {
                covered_to = spans[i].e;
                after = named_entry(spans[i].entry);
            }
        }
        if (frame_end > covered_to) {
            const uint64_t g = frame_end - covered_to;
            gap += g;
            if (g > largest) {
                largest = g;
                x.gap_after = after;
                x.gap_before = -1;
            }
        }
        x.gap_ms = (float)((double)gap * ms_per_tick);
        x.gap_largest_ms = (float)((double)largest * ms_per_tick);
    }

    // the ray traced sound's last run, while runs are recent (its own queue's clock:
    // only its length is meaningful, so it is laid from the frame's start)
    float sound_ms = 0.f;
    if (d3d12_sound_last_run(&sound_ms) && sound_ms > 0.f && frame_start) {
        ticks[PROFILER_SOUND_TRACE * 2] = frame_start;
        ticks[PROFILER_SOUND_TRACE * 2 + 1] = frame_start + max((uint64_t)1, (uint64_t)((double)sound_ms / ms_per_tick));
        used[PROFILER_SOUND_TRACE * 2] = used[PROFILER_SOUND_TRACE * 2 + 1] = true;
    }

    vkpt_profiler_take_extras(&x);
}

// the slot's last frame, now that its fence has passed, to vkpt's profiler
static void collect(int slot)
{
    bool *used = t.used[slot];
    uint64_t ticks[D3D12_PROFILER_QUERIES];
    memset(ticks, 0, sizeof(ticks));

    void *map = NULL;
    const SIZE_T first = (SIZE_T)slot * D3D12_PROFILER_QUERIES * sizeof(uint64_t);
    const D3D12_RANGE range = { first, first + D3D12_PROFILER_QUERIES * sizeof(uint64_t) };
    if (t.resolved[slot] && SUCCEEDED(ID3D12Resource_Map(t.readback, 0, &range, &map))) {
        const uint64_t *in = (const uint64_t *)((const uint8_t *)map + first);
        // the rest of the slot holds older frames' values
        for (int q = 0; q < D3D12_PROFILER_QUERIES; q++)
            if (used[q])
                ticks[q] = in[q];
        const D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Unmap(t.readback, 0, &none);
    } else {
        memset(used, 0, sizeof(t.used[slot]));
    }
    t.resolved[slot] = false;

    // before the second pieces move the ends
    account(ticks, used, slot, t.ns_per_tick);

    // the second pieces onto their entries' ends
    for (int i = 0; i < D3D12_NUM_PROFILER_ENTRIES - NUM_PROFILER_ENTRIES; i++) {
        const uint64_t *x = ticks + (NUM_PROFILER_ENTRIES + i) * 2;
        uint64_t *y = ticks + extra_target[i] * 2;
        if (x[0] && x[1] >= x[0] && y[0] && y[1])
            y[1] += x[1] - x[0];
    }

    vkpt_profiler_take_results(ticks, used, t.ns_per_tick);
    memset(used, 0, sizeof(t.used[slot]));
}

void d3d12_timers_frame_begin(void)
{
    if (!timers_on())
        return;
    const int slot = d3d.frame_index;
    collect(slot);
    t.begin_us[slot] = Sys_Microseconds();
    t.submit_us[slot] = 0;
    t.serial[slot] = cpuprof_last.serial + 1;   // this frame closes as the next one
    t.uploads = 0;
    // the slot's stamps are done with (its fence passed)
    if (FAILED(ID3D12CommandAllocator_Reset(t.allocator[slot])))
        return;
    stamp_on_queue(PROFILER_GPU_FRAME, PROFILER_START);
    // and the list's first command: where the GPU starts on it (GPU_IDLE, collect)
    d3d12_timers_mark(PROFILER_GPU_IDLE, PROFILER_STOP);
}

void d3d12_timers_mark(int entry, int action)
{
    d3d12_timers_t tm;
    d3d12_timers_get(&tm);
    if (tm.heap)
        d3d12_timer_mark(d3d.cmd, &tm, entry, action);
}

/* An upload list executed on the queue while a frame is open (d3d12_upload_flush):
   stamps on the queue either side of it, the first into UPLOAD_QUEUE and the next
   three into its second pieces; past four, the last one's end moves on. */
void d3d12_timers_upload_flush(int action)
{
    if (!timers_on() || !d3d.frame_open)
        return;
    if (action == PROFILER_START) {
        if (t.uploads < 4) {
            const int k = t.uploads++;
            stamp_on_queue(k ? D3D12_PROFILER_UPLOAD_QUEUE_2 + k - 1 : PROFILER_UPLOAD_QUEUE, PROFILER_START);
        }
    } else if (t.uploads > 0) {
        const int k = t.uploads - 1;
        stamp_on_queue(k ? D3D12_PROFILER_UPLOAD_QUEUE_2 + k - 1 : PROFILER_UPLOAD_QUEUE, PROFILER_STOP);
    }
}

void d3d12_timers_submitted(void)
{
    if (t.heap)
        t.submit_us[d3d.frame_index] = Sys_Microseconds();
}

void d3d12_timers_get(d3d12_timers_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!t.heap || !d3d.frame_open)
        return;
    const int slot = d3d.frame_index;
    out->heap = t.heap;
    out->base = (uint32_t)(slot * D3D12_PROFILER_QUERIES);
    out->used = t.used[slot];
    out->markers = d3d.dred >= 2;
}

void d3d12_timers_flow(int action)
{
    if (timers_on())
        stamp_on_queue(PROFILER_FLOW_SIMULATION, action);
}

void d3d12_timers_frame_end(ID3D12GraphicsCommandList4 *cmd)
{
    d3d12_timers_t tm;
    d3d12_timers_get(&tm);
    if (!tm.heap)
        return;
    d3d12_timer_mark(cmd, &tm, PROFILER_GPU_FRAME, PROFILER_STOP);

    // only what was written this frame - a query never ended has nothing to resolve
    for (int q = 0; q < D3D12_PROFILER_QUERIES;) {
        if (!tm.used[q]) {
            q++;
            continue;
        }
        int end = q;
        while (end < D3D12_PROFILER_QUERIES && tm.used[end])
            end++;
        ID3D12GraphicsCommandList4_ResolveQueryData(cmd, t.heap, D3D12_QUERY_TYPE_TIMESTAMP, tm.base + (UINT)q,
                                                    (UINT)(end - q), t.readback,
                                                    (UINT64)(tm.base + (UINT)q) * sizeof(uint64_t));
        q = end;
    }
    t.resolved[d3d.frame_index] = true;
}

void d3d12_timers_draw(void)
{
    if (!t.profiler) {
        t.profiler = Cvar_Get("profiler", "0", 0);
        t.ser = Cvar_Get("pt_ser", "0", 0);
    }
    if (!t.profiler->integer)
        return;

    // vkpt's table, as R_EndFrame_RTX draws it, titled with which indirect
    // lighting pass the SER A/B is timing (draw_profiler places the line)
    char buf[128];
    const int ser = t.ser->integer;
    Q_snprintf(buf, sizeof(buf), "D3D12, pt_ser %d: %s", ser,
               ser == 1 ? "ray generation + MaybeReorderThread" : ser == 2 ? "ray generation, no reorder" :
               ser == 3 ? "ray generation + MaybeReorderThread before the triangle fetch" : "compute");
    draw_profiler(Cvar_VariableInteger("flt_enable") != 0, buf);
}

void d3d12_timers_shutdown(void)
{
    // the GPU is idle here
    release();
    t.failed = false;
    memset(t.resolved, 0, sizeof(t.resolved));
    memset(t.used, 0, sizeof(t.used));
    // the averages and their buffers: the next renderer starts its own
    vkpt_profiler_release();
}
