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
*/

#include "d3d12pt.h"
#include "passes.h"

// passes.h's second pieces, in order: the entry each is added to
static const int extra_target[] = {
    PROFILER_SHADOW_MAP,            // D3D12_PROFILER_SHADOW_MAP_DYNAMIC
    PROFILER_BVH_UPDATE,            // D3D12_PROFILER_BVH_TLAS
    PROFILER_FLOW_SIMULATION,       // D3D12_PROFILER_FLOW_PASSES
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
    ID3D12GraphicsCommandList_EndQuery(t.list, t.heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                       (UINT)(slot * D3D12_PROFILER_QUERIES) + q);
    if (FAILED(ID3D12GraphicsCommandList_Close(t.list)))
        return;
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)t.list };
    ID3D12CommandQueue_ExecuteCommandLists(d3d.queue, 1, lists);
    t.used[slot][q] = true;
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
    // the slot's stamps are done with (its fence passed)
    if (FAILED(ID3D12CommandAllocator_Reset(t.allocator[slot])))
        return;
    stamp_on_queue(PROFILER_GPU_FRAME, PROFILER_START);
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
