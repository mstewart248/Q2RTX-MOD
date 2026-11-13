// Headless NVIDIA Flow benchmark for the plasma beam and the railgun trail,
// the two Flow effects that cost the most frame time. It drives Flow the way
// vkpt/flow.c does (same layers, cell sizes, layer settings and emitter
// shapes, worked out from flow_layer_simulate, flow_heatbeam and
// flow_rail_piece), on a D3D12 device made as d3d12/device.c makes it, with
// Flow's own profiler on: GPU time per Flow pass, Flow's CPU time, blocks.
//
// It also dumps the gas the renderer would draw at the last frame - every
// cell of every block in use, by layer and world cell, so that a variant can
// be checked against the game's settings cell by cell (--compare).
//
// From C:/Q2RTX, after setup/build_flow.py:
//   cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I extern/agility-sdk/include
//      /I extern/flow/include/nvflow /I extern/flow/include/nvflowext
//      src/refresh/d3d12/tests/d3d12_flow_bench.c /Fe:build/tools/ /Fo:build/tools/
//   build/tools/d3d12_flow_bench.exe C:/Q2RTX/ beam|beamfire|rail [variant] [dump.bin]
//   build/tools/d3d12_flow_bench.exe --compare a.bin b.bin
//
// variant letters (none = the game as it is):
//   a  plasma beam layers: no blocks kept by Flow's summary (emitters only)
//   n  plasma beam layers: no neighbour blocks
//   c  plasma beam layers: no core simulation, density faded only
//   p  plasma beam layers: no pressure or vorticity
//   r  rail layers: no neighbour blocks (NOT lossless - for scale only)
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

#define NV_FLOW_CPU 1
#include "NvFlowExt.h"
#include "NvFlowD3D12External.h"

// shader/flow.h
#define LAYERS              9
#define L_FIRE              0
#define L_RAIL              3
#define L_HEATBEAM          4
#define L_RAIL_FAR          5
#define L_FIRE_TRAIL        6
#define L_HEATBEAM_VIEW     7
#define L_HEATBEAM_MIRROR   8
#define IS_HEATBEAM(l)      ((l) == L_HEATBEAM || (l) == L_HEATBEAM_VIEW || (l) == L_HEATBEAM_MIRROR)
#define IS_RAIL(l)          ((l) == L_RAIL || (l) == L_RAIL_FAR)

#define MAX_BOXES           1024
#define MAX_SPHERES         64
#define CHORDS              8

// Matt's rerelease/q2config.cfg (11-12)
#define CELL_FIRE           3.f
#define CELL_RAIL           0.75f
#define CELL_RAIL_FAR       2.f
#define CELL_HEATBEAM       1.f
#define POOL                3584
#define COUPLE_RATE         30.f

static int errors, flow_errors, flow_warnings;
static ID3D12Device *dev;

static void __stdcall msg(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id, LPCSTR d, void *ctx)
{
    (void)c; (void)ctx; (void)id;
    if (s <= D3D12_MESSAGE_SEVERITY_ERROR) {
        errors++;
        if (errors < 20)
            printf("  [d3d12 error] %s\n", d);
    }
}

static void NV_FLOW_ABI flow_log(NvFlowLogLevel level, const char *format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (level == eNvFlowLogLevel_error)
        flow_errors++;
    else if (level == eNvFlowLogLevel_warning)
        flow_warnings++;
    if (level != eNvFlowLogLevel_info && flow_errors + flow_warnings < 20)
        printf("  [flow %s] %s\n", level == eNvFlowLogLevel_error ? "error" : "warning", buf);
}

static float half_to_float(uint16_t h)
{
    const uint32_t e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0)
        v = ldexpf((float)m, -24);
    else if (e == 31)
        v = m ? NAN : INFINITY;
    else
        v = ldexpf((float)(m | 1024), (int)e - 25);
    return (h & 0x8000) ? -v : v;
}

static FARPROC get(HMODULE lib, const char *name)
{
    FARPROC p = GetProcAddress(lib, name);
    if (!p)
        printf("missing export %s\n", name);
    return p;
}

// ---------------------------------------------------------------- profiler

#define MAX_LABELS 256
static struct {
    const char *label;
    double gpu, cpu;
    uint64_t count;
} labels[MAX_LABELS];
static int num_labels;
static int measuring;
static uint64_t captures;

static void NV_FLOW_ABI report(void *userdata, NvFlowUint64 capture_id, NvFlowUint num, NvFlowProfilerEntry *entries)
{
    (void)userdata; (void)capture_id;
    if (!measuring)
        return;
    captures++;
    for (NvFlowUint i = 0; i < num; i++) {
        const char *l = entries[i].label ? entries[i].label : "?";
        int k;
        for (k = 0; k < num_labels; k++)
            if (!strcmp(labels[k].label, l))
                break;
        if (k == num_labels) {
            if (num_labels == MAX_LABELS)
                continue;
            labels[num_labels++].label = _strdup(l);
        }
        labels[k].gpu += entries[i].gpuDeltaTime;
        labels[k].cpu += entries[i].cpuDeltaTime;
        labels[k].count++;
    }
}

static int by_gpu(const void *a, const void *b)
{
    const double x = ((const double *)a)[1], y = ((const double *)b)[1];
    return x < y ? 1 : x > y ? -1 : 0;
}

// ---------------------------------------------------------------- vectors

typedef float vec3[3];
static void vset(vec3 v, float x, float y, float z) { v[0] = x; v[1] = y; v[2] = z; }
static void vma(const vec3 a, float s, const vec3 b, vec3 o) { o[0] = a[0] + s * b[0]; o[1] = a[1] + s * b[1]; o[2] = a[2] + s * b[2]; }
static float vdot(const vec3 a, const vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void vcross(const vec3 a, const vec3 b, vec3 o) { vec3 t = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; memcpy(o, t, sizeof(t)); }
static float vnorm(vec3 v) { const float l = sqrtf(vdot(v, v)); if (l > 0.f) { v[0] /= l; v[1] /= l; v[2] /= l; } return l; }
static float vdist(const vec3 a, const vec3 b) { vec3 d = { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; return sqrtf(vdot(d, d)); }

// Quake's MakeNormalVectors
static void make_normals(const vec3 forward, vec3 right, vec3 up)
{
    right[1] = -forward[0];
    right[2] = forward[1];
    right[0] = forward[2];
    const float d = vdot(right, forward);
    vma(right, -d, forward, right);
    vnorm(right);
    vcross(right, forward, up);
}

// ---------------------------------------------------------------- emitters

static NvFlowGridEmitterBoxParams boxes[MAX_BOXES];
static NvFlowGridEmitterBoxParams *box_ptrs[MAX_BOXES];
static int num_boxes;
static NvFlowGridEmitterSphereParams spheres[MAX_SPHERES];
static NvFlowGridEmitterSphereParams *sphere_ptrs[MAX_SPHERES];
static int num_spheres;

// flow.c flow_line_box
static NvFlowGridEmitterBoxParams *line_box(uint64_t luid, int layer, const vec3 a, const vec3 b, float half_width, float pad)
{
    if (num_boxes >= MAX_BOXES)
        return NULL;
    vec3 dir = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, side, up;
    const float length = vnorm(dir);
    if (length < 0.01f)
        return NULL;
    make_normals(dir, side, up);
    vcross(dir, side, up);
    NvFlowGridEmitterBoxParams *box = &boxes[num_boxes];
    *box = NvFlowEmitterBoxParams_default;
    box->luid = luid;
    box->layer = layer;
    const NvFlowFloat4x4 to_world = {
        dir[0], dir[1], dir[2], 0.f,
        side[0], side[1], side[2], 0.f,
        up[0], up[1], up[2], 0.f,
        0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2]), 1.f
    };
    box->localToWorld = to_world;
    box->localToWorldVelocity = to_world;
    box->velocityIsWorldSpace = NV_FLOW_TRUE;
    box->position.x = box->position.y = box->position.z = 0.f;
    box->halfSize.x = 0.5f * length + pad;
    box->halfSize.y = box->halfSize.z = half_width;
    box->velocity.x = box->velocity.y = box->velocity.z = 0.f;
    box->multisample = NV_FLOW_TRUE;
    box->coupleRateVelocity = box->coupleRateDivergence = 0.f;
    box->coupleRateTemperature = box->coupleRateFuel = box->coupleRateBurn = box->coupleRateSmoke = 0.f;
    box_ptrs[num_boxes++] = box;
    return box;
}

static NvFlowGridEmitterSphereParams *sphere(uint64_t luid, int layer, const vec3 p, float radius)
{
    if (num_spheres >= MAX_SPHERES)
        return NULL;
    NvFlowGridEmitterSphereParams *s = &spheres[num_spheres];
    *s = NvFlowEmitterSphereParams_default;
    s->luid = luid;
    s->layer = layer;
    s->position.x = p[0]; s->position.y = p[1]; s->position.z = p[2];
    s->radius = radius;
    s->multisample = NV_FLOW_TRUE;
    s->velocity.x = s->velocity.y = s->velocity.z = 0.f;
    sphere_ptrs[num_spheres++] = s;
    return s;
}

static const vec3 view_origin = { 0.f, 0.f, 0.f };

/* One plasma beam on `layer`, as flow_heatbeam lays it with no trails
   (pt_flow_heatbeam_trails 0, Matt's): core pieces with their hotter middle,
   two strands over the first 256 units, rings out from `clear`, a hot spot at
   the end. Defaults of 11-12. */
static void heatbeam(int layer, const vec3 origin, const vec3 end, float from, float clear, float age, float step_time)
{
    vec3 dir = { end[0] - origin[0], end[1] - origin[1], end[2] - origin[2] }, right, up;
    const float length = vnorm(dir);
    make_normals(dir, right, up);
    const uint64_t luid = (uint64_t)(layer + 1) << 20;
    const float cell = CELL_HEATBEAM;
    const float rate = fmaxf(5.f * COUPLE_RATE, 4.f / step_time);   // hard
    const float core_rate = 1.f / step_time;                          // soft fill
    const float to = length;

    // core
    const float core_radius = 1.5f;
    struct { vec3 a, b; } pieces[256];
    int num_pieces = 0;
    float s = from;
    for (uint64_t piece = 1; s < to - 0.01f; piece++) {
        vec3 a, b;
        vma(origin, s, dir, a);
        const float step = fminf(fmaxf(0.25f * vdist(a, view_origin), 2.f * cell), 16.f * cell);
        float t = fminf(s + step, to);
        if (to - t < 0.5f * step)
            t = to;
        vma(origin, t, dir, b);
        s = t;
        NvFlowGridEmitterBoxParams *box = line_box(luid | (piece << 32), layer, a, b, core_radius, 0.f);
        if (!box)
            return;
        box->velocity.x = dir[0] * 100.f; box->velocity.y = dir[1] * 100.f; box->velocity.z = dir[2] * 100.f;
        box->coupleRateVelocity = core_rate;
        box->divergence = 2.f * 10.f * (2.f * cell) / fmaxf(core_radius, cell);
        box->temperature = 0.88f;
        box->fuel = 1.25f;
        box->burn = 0.f;
        box->smoke = 0.15f;
        box->coupleRateDivergence = box->coupleRateTemperature = box->coupleRateFuel = box->coupleRateSmoke = core_rate;
        if (num_pieces < 256) {
            memcpy(pieces[num_pieces].a, a, sizeof(vec3));
            memcpy(pieces[num_pieces].b, b, sizeof(vec3));
            num_pieces++;
        }
    }

    // rings: spacing 225, speed 225, travel 384, radius 4, width 1
    const float ring_from = fminf(fmaxf(from, clear), to);
    const float ring_to = fminf(to, ring_from + 384.f);
    const double run = (double)age * 225.0;
    const int newest = (int)floor(run / 225.0);
    for (int n = newest - (int)floor((ring_to - fmod(run, 225.0)) / 225.0); n <= newest; n++) {
        const float rs = (float)(run - (double)n * 225.0);
        if (rs < ring_from || rs > ring_to)
            continue;
        const float r = 4.f * fminf(rs / 10.f, 1.f);
        if (r < 0.5f)
            continue;
        vec3 centre;
        vma(origin, rs, dir, centre);
        for (int c = 0; c < CHORDS; c++) {
            const float a0 = 2.f * 3.14159265f * c / CHORDS, a1 = 2.f * 3.14159265f * (c + 1) / CHORDS;
            vec3 a, b;
            vma(centre, r * cosf(a0), right, a); vma(a, r * sinf(a0), up, a);
            vma(centre, r * cosf(a1), right, b); vma(b, r * sinf(a1), up, b);
            NvFlowGridEmitterBoxParams *box = line_box(luid | (1ull << 56) | ((uint64_t)((n & 1023) * CHORDS + c + 1) << 32),
                                                       layer, a, b, 1.f, 0.5f);
            if (!box)
                return;
            box->velocity.x = dir[0] * 225.f; box->velocity.y = dir[1] * 225.f; box->velocity.z = dir[2] * 225.f;
            box->coupleRateVelocity = rate;
            box->divergence = 2.f * 4.f * (2.f * cell) / fmaxf(1.f, cell);
            box->temperature = 1.1f;
            box->fuel = 1.f;
            box->smoke = 0.3f;
            box->coupleRateDivergence = box->coupleRateTemperature = box->coupleRateFuel = box->coupleRateSmoke = rate;
        }
    }

    // hot spot
    if (vdist(end, view_origin) > 32.f) {
        vec3 p;
        vma(end, -0.5f * 6.f, dir, p);
        NvFlowGridEmitterSphereParams *sp = sphere(luid | (1ull << 48), layer, p, 6.f);
        if (sp) {
            sp->divergence = 3.f * 4.f * (2.f * cell) / 6.f;
            sp->temperature = 1.f;
            sp->fuel = 1.f;
            sp->burn = 0.f;
            sp->smoke = 0.3f;
            sp->coupleRateVelocity = 0.f;
            sp->coupleRateDivergence = sp->coupleRateTemperature = sp->coupleRateFuel = sp->coupleRateSmoke = rate;
            sp->coupleRateBurn = 0.f;
        }
    }

    // hotter middle
    for (int i = 0; i < num_pieces; i++) {
        NvFlowGridEmitterBoxParams *box = line_box(luid | (1ull << 57) | ((uint64_t)(i + 1) << 32), layer,
                                                   pieces[i].a, pieces[i].b, 0.5f * core_radius, 0.f);
        if (!box)
            return;
        box->temperature = 1.06f;
        box->fuel = 1.25f;
        box->coupleRateTemperature = box->coupleRateFuel = core_rate;
    }

    // strands: 2, pitch 48, radius 0.6, half width 0.4, over 256 units
    const float strand_to = fminf(to, from + 256.f);
    const float chord = 48.f / 8.f;
    const int first = (int)floorf(from / chord);
    for (int k = 0; k < 2; k++)
        for (int i = first; (float)i * chord < strand_to; i++) {
            const float s0 = fmaxf((float)i * chord, from), s1 = fminf((float)(i + 1) * chord, strand_to);
            if (s1 - s0 < 0.01f)
                continue;
            vec3 a, b;
            for (int e = 0; e < 2; e++) {
                const float ss = e ? s1 : s0;
                const double turns = ((double)ss - run) / 48.0 + (double)k / 2;
                const float angle = (float)(2.0 * 3.14159265358979 * (turns - floor(turns)));
                float *p = e ? b : a;
                vma(origin, ss, dir, p);
                vma(p, 0.9f * cosf(angle), right, p);
                vma(p, 0.9f * sinf(angle), up, p);
            }
            NvFlowGridEmitterBoxParams *box = line_box(luid | (1ull << 54) | ((uint64_t)(k * 4096 + (i - first) + 1) << 32),
                                                       layer, a, b, 0.4f, 0.4f);
            if (!box)
                return;
            box->temperature = 1.19f;
            box->fuel = 1.25f;
            box->coupleRateTemperature = box->coupleRateFuel = core_rate;
        }
}

/* A rail shot's boxes (flow_rail_piece), emitted for its 0.05 s burst:
   the core line and the helix, fine cells within 256 units of the view, the
   far grid past that. */
static void rail_piece(const vec3 origin, const vec3 dir, float from, float to, int piece, int layer, float cell)
{
    vec3 right, up;
    make_normals(dir, right, up);
    const uint64_t luid = (0x5ull << 20) | ((uint64_t)piece << 57);
    const float rate = 5.f * COUPLE_RATE;
    {
        const float radius = 1.5f;
        const int segments = (int)fmaxf(ceilf((to - from) / 48.f), 1.f);
        for (int s = 0; s < segments; s++) {
            vec3 a, b;
            vma(origin, from + (to - from) * s / segments, dir, a);
            vma(origin, from + (to - from) * (s + 1) / segments, dir, b);
            NvFlowGridEmitterBoxParams *box = line_box(luid | ((uint64_t)(s + 1) << 32), layer, a, b, radius, 0.f);
            if (!box)
                return;
            box->divergence = 2.f * 30.f * (2.f * cell) / fmaxf(radius, cell);
            box->temperature = 0.3f;
            box->smoke = 1.f;
            box->coupleRateDivergence = box->coupleRateTemperature = box->coupleRateFuel = box->coupleRateSmoke = rate;
        }
    }
    const float radius = 6.f, width = 1.25f, turn = 63.f, step = turn / CHORDS, k = 2.f * 3.14159265f / turn, swirl = 15.f;
    const int first = (int)floorf(from / step), last = (int)ceilf(to / step);
    for (int i = first; i < last; i++) {
        const float s0 = fmaxf(i * step, from), s1 = fminf((i + 1) * step, to);
        if (s1 - s0 < 0.5f)
            continue;
        vec3 a, b;
        vma(origin, s0, dir, a); vma(a, radius * cosf(k * s0), right, a); vma(a, radius * sinf(k * s0), up, a);
        vma(origin, s1, dir, b); vma(b, radius * cosf(k * s1), right, b); vma(b, radius * sinf(k * s1), up, b);
        NvFlowGridEmitterBoxParams *box = line_box(luid | (1ull << 56) | ((uint64_t)(i + 1) << 32), layer, a, b, width, 0.5f * width);
        if (!box)
            return;
        const float angle = k * 0.5f * (s0 + s1);
        vec3 v = { 0, 0, 0 };
        vma(v, swirl * cosf(angle), up, v);
        vma(v, -swirl * sinf(angle), right, v);
        box->velocity.x = v[0]; box->velocity.y = v[1]; box->velocity.z = v[2];
        box->coupleRateVelocity = rate;
        box->divergence = 2.f * 4.f * (2.f * cell) / fmaxf(width, cell);
        box->temperature = 0.85f;
        box->smoke = 0.5f;
        box->coupleRateDivergence = box->coupleRateTemperature = box->coupleRateFuel = box->coupleRateSmoke = rate;
    }
}

// ---------------------------------------------------------------- layers

static NvFlowGridSimulateLayerParams sim[LAYERS];
static NvFlowGridSimulateLayerParams *sim_ptrs[LAYERS];

static float cell_size(int l)
{
    return l == L_RAIL ? CELL_RAIL : l == L_RAIL_FAR ? CELL_RAIL_FAR : IS_HEATBEAM(l) ? CELL_HEATBEAM : CELL_FIRE;
}

// flow.c flow_layer_simulate, for the layers this bench uses
static void layer_simulate(int l, int every_frame, float rail_age, const char *variant)
{
    NvFlowGridSimulateLayerParams *s = &sim[l];
    *s = NvFlowGridSimulateLayerParams_default;
    s->luid = 1 + l;
    s->layer = l;
    s->densityCellSize = cell_size(l);
    if (every_frame) {
        s->enableVariableTimeStep = NV_FLOW_TRUE;
        s->maxStepsPerSimulate = 1;
    }
    else {
        s->enableVariableTimeStep = NV_FLOW_FALSE;
        s->stepsPerSecond = 60.f;
        s->maxStepsPerSimulate = 1;
    }
    s->vorticity.forceScale = 0.6f;
    s->physicsCollisionEnabled = NV_FLOW_FALSE;
    NvFlowAdvectionCombustionParams *a = &s->advection;
    if (l == L_FIRE || l == L_FIRE_TRAIL) {
        a->buoyancyPerTemp = 2.f;
        a->coolingRate = 1.3f;
        a->smoke.damping = 0.5f;
        a->fuel.fade = 0.25f;
    }
    else if (IS_RAIL(l)) {
        a->buoyancyPerTemp = 0.5f;
        a->coolingRate = 2.5f;
        a->smoke.damping = 0.8f;
        s->vorticity.forceScale = 2.f;
        a->velocity.damping = 0.5f;
        s->summaryAllocate.speedThresholdMinSmoke = s->summaryAllocate.smokeThreshold;
        // flow_rail_fade: drained to nothing over the last 0.4 s of 1.4
        a->smoke.fade = rail_age < 1.f ? 0.4f : 2.5f;
        a->temperature.fade = rail_age < 1.f ? 0.f : 2.5f;
        if (strchr(variant, 'r'))
            s->summaryAllocate.enableNeighborAllocation = NV_FLOW_FALSE;
    }
    else if (IS_HEATBEAM(l)) {
        a->ignitionTemp = 1e6f;
        a->burnPerTemp = 0.f;
        a->buoyancyPerTemp = 1.f;
        a->coolingRate = 4.f;
        a->smoke.fade = 0.5f;
        s->vorticity.forceScale = 2.f;
        a->velocity.damping = 0.5f;
        s->summaryAllocate.speedThresholdMinSmoke = s->summaryAllocate.smokeThreshold;
        a->temperature.damping = 1.f;   // no trails
        a->fuel.damping = 1.f;
        a->smoke.damping = 1.f;
        if (strchr(variant, 'a'))
            s->summaryAllocate.enabled = NV_FLOW_FALSE;
        if (strchr(variant, 'n'))
            s->summaryAllocate.enableNeighborAllocation = NV_FLOW_FALSE;
        if (strchr(variant, 'c')) {
            s->forceDisableCoreSimulation = NV_FLOW_TRUE;
            a->forceFadeEnabled = NV_FLOW_TRUE;
        }
        if (strchr(variant, 'p')) {
            s->pressure.enabled = NV_FLOW_FALSE;
            s->vorticity.enabled = NV_FLOW_FALSE;
        }
    }
}

// ---------------------------------------------------------------- gas dump

typedef struct {
    int32_t layer, x, y, z;
    float v[4];
} cell_t;

static int cell_cmp(const void *pa, const void *pb)
{
    const cell_t *a = pa, *b = pb;
    if (a->layer != b->layer) return a->layer < b->layer ? -1 : 1;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return 0;
}

static void *read_back(ID3D12CommandQueue *queue, ID3D12Resource *res, D3D12_RESOURCE_STATES state, int is_texture,
                       D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp_out, ID3D12Resource **rb_out)
{
    D3D12_RESOURCE_DESC rdesc;
    ID3D12Resource_GetDesc(res, &rdesc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = { 0 };
    UINT rows; UINT64 row_size, total;
    if (is_texture)
        ID3D12Device_GetCopyableFootprints(dev, &rdesc, 0, 1, 0, &fp, &rows, &row_size, &total);
    else
        total = rdesc.Width;

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = { 0 };
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                         NULL, &IID_ID3D12Resource, (void **)&rb);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12GraphicsCommandList *cl = NULL;
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl);
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (state != D3D12_RESOURCE_STATE_COMMON || is_texture)
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    if (is_texture) {
        D3D12_TEXTURE_COPY_LOCATION src = { res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint = fp;
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = state;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    }
    else {
        // buffers in COMMON decay/promote implicitly
        ID3D12GraphicsCommandList_CopyBufferRegion(cl, rb, 0, res, 0, total);
    }
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
    ID3D12Fence *fence = NULL;
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
    ID3D12Fence_Release(fence);
    ID3D12GraphicsCommandList_Release(cl);
    ID3D12CommandAllocator_Release(ca);
    void *p = NULL;
    D3D12_RANGE range = { 0, (SIZE_T)total };
    ID3D12Resource_Map(rb, 0, &range, &p);
    if (fp_out)
        *fp_out = fp;
    *rb_out = rb;
    return p;
}

static int compare(const char *pa, const char *pb)
{
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    if (!fa || !fb) {
        printf("cannot open %s\n", !fa ? pa : pb);
        return 2;
    }
    uint64_t na = 0, nb = 0;
    fread(&na, 8, 1, fa);
    fread(&nb, 8, 1, fb);
    cell_t *a = malloc(sizeof(cell_t) * (na + 1)), *b = malloc(sizeof(cell_t) * (nb + 1));
    fread(a, sizeof(cell_t), na, fa);
    fread(b, sizeof(cell_t), nb, fb);
    fclose(fa); fclose(fb);

    uint64_t i = 0, j = 0, only_a = 0, only_b = 0, both = 0, differ = 0;
    double max_diff = 0.0, sum_a[LAYERS][4] = { 0 }, sum_b[LAYERS][4] = { 0 };
    float only_peak = 0.f;
    while (i < na || j < nb) {
        const int c = i == na ? 1 : j == nb ? -1 : cell_cmp(&a[i], &b[j]);
        if (c < 0) {
            only_a++;
            for (int k = 0; k < 4; k++) only_peak = fmaxf(only_peak, fabsf(a[i].v[k]));
            if (a[i].layer >= 0 && a[i].layer < LAYERS) for (int k = 0; k < 4; k++) sum_a[a[i].layer][k] += a[i].v[k];
            i++;
        }
        else if (c > 0) {
            only_b++;
            for (int k = 0; k < 4; k++) only_peak = fmaxf(only_peak, fabsf(b[j].v[k]));
            if (b[j].layer >= 0 && b[j].layer < LAYERS) for (int k = 0; k < 4; k++) sum_b[b[j].layer][k] += b[j].v[k];
            j++;
        }
        else {
            both++;
            double d = 0.0;
            for (int k = 0; k < 4; k++) {
                d = fmax(d, fabs((double)a[i].v[k] - b[j].v[k]));
                if (a[i].layer >= 0 && a[i].layer < LAYERS) {
                    sum_a[a[i].layer][k] += a[i].v[k];
                    sum_b[b[j].layer][k] += b[j].v[k];
                }
            }
            if (d > 1e-3)
                differ++;
            max_diff = fmax(max_diff, d);
            i++; j++;
        }
    }
    printf("cells with gas: %llu vs %llu; in both %llu (%llu differ by > 1e-3, largest difference %.5f); "
           "only in one: %llu + %llu (largest value %.5f)\n",
           (unsigned long long)na, (unsigned long long)nb, (unsigned long long)both, (unsigned long long)differ, max_diff,
           (unsigned long long)only_a, (unsigned long long)only_b, only_peak);
    for (int l = 0; l < LAYERS; l++) {
        if (sum_a[l][0] == 0 && sum_a[l][1] == 0 && sum_a[l][3] == 0 && sum_b[l][0] == 0 && sum_b[l][1] == 0 && sum_b[l][3] == 0)
            continue;
        printf("  layer %d sums: temperature %.2f / %.2f, fuel %.2f / %.2f, smoke %.2f / %.2f\n", l,
               sum_a[l][0], sum_b[l][0], sum_a[l][1], sum_b[l][1], sum_a[l][3], sum_b[l][3]);
    }
    return 0;
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[1], "--compare"))
        return compare(argv[2], argv[3]);

    const char *root = argc > 1 ? argv[1] : "C:/Q2RTX/";
    const char *scenario = argc > 2 ? argv[2] : "beam";
    const char *variant = argc > 3 ? argv[3] : "";
    const char *dump = argc > 4 ? argv[4] : NULL;
    if (!strcmp(variant, "-"))
        variant = "";
    const int is_beam = !strncmp(scenario, "beam", 4);
    const int with_fire = !strcmp(scenario, "beamfire");
    const int is_rail = !strcmp(scenario, "rail");
    if (!is_beam && !is_rail) {
        printf("scenario: beam, beamfire or rail\n");
        return 2;
    }
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE getif = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");
    ID3D12SDKConfiguration1 *cfg = NULL;
    getif(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg);
    ID3D12DeviceFactory *f = NULL;
    snprintf(path, sizeof(path), "%sD3D12\\", root);
    if (FAILED(ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, D3D12_SDK_VERSION, path, &IID_ID3D12DeviceFactory, (void **)&f))) {
        printf("no Agility runtime at %s\n", path);
        return 1;
    }
    IDXGIFactory6 *xf = NULL; mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    if (FAILED(ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev))) {
        printf("CreateDevice failed\n");
        return 1;
    }
    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        DWORD cookie; ID3D12InfoQueue1_RegisterMessageCallback(iq, msg, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    }
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue = NULL;
    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    ID3D12Device_SetStablePowerState(dev, FALSE);

    snprintf(path, sizeof(path), "%snvflow.dll", root);
    HMODULE lib = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    snprintf(path, sizeof(path), "%snvflowext.dll", root);
    HMODULE ext = lib ? LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
    if (!lib || !ext) {
        printf("could not load %s (error %lu)\n", path, GetLastError());
        return 1;
    }
    PFN_NvFlowGetOpList get_op_list = (PFN_NvFlowGetOpList)get(lib, "NvFlowGetOpList");
    PFN_NvFlowGetExtOpList get_ext_op_list = (PFN_NvFlowGetExtOpList)get(ext, "NvFlowGetExtOpList");
    PFN_NvFlowGetGridInterface get_grid = (PFN_NvFlowGetGridInterface)get(ext, "NvFlowGetGridInterface");
    PFN_NvFlowGetGridParamsInterface get_params = (PFN_NvFlowGetGridParamsInterface)get(ext, "NvFlowGetGridParamsInterface");
    PFN_NvFlowGetDeviceInterface get_device = (PFN_NvFlowGetDeviceInterface)get(ext, "NvFlowGetDeviceInterface");
    PFN_NvFlowD3D12ExternalCreateDeviceManager create_manager =
        (PFN_NvFlowD3D12ExternalCreateDeviceManager)get(ext, "NvFlowD3D12ExternalCreateDeviceManager");
    PFN_NvFlowD3D12ExternalCreateDevice create_device = (PFN_NvFlowD3D12ExternalCreateDevice)get(ext, "NvFlowD3D12ExternalCreateDevice");
    PFN_NvFlowD3D12ExternalGetTexture get_texture = (PFN_NvFlowD3D12ExternalGetTexture)get(ext, "NvFlowD3D12ExternalGetTexture");
    PFN_NvFlowD3D12ExternalGetBuffer get_buffer = (PFN_NvFlowD3D12ExternalGetBuffer)get(ext, "NvFlowD3D12ExternalGetBuffer");
    if (!get_op_list || !get_ext_op_list || !get_grid || !get_params || !get_device ||
        !create_manager || !create_device || !get_texture || !get_buffer)
        return 1;

    NvFlowOpList *op_list = get_op_list();
    NvFlowExtOpList *ext_op_list = get_ext_op_list();
    NvFlowGridInterface *gi = get_grid();
    NvFlowGridParamsInterface *pi = get_params();
    NvFlowDeviceInterface *di = get_device(eNvFlowContextApi_d3d12);
    NvFlowD3D12ExternalDesc desc = { dev, queue, flow_log };
    NvFlowDeviceManager *manager = create_manager(&desc);
    NvFlowDevice *device = manager ? create_device(manager, &desc) : NULL;
    if (!device) {
        printf("NvFlowD3D12ExternalCreateDevice failed\n");
        return 1;
    }
    NvFlowDeviceQueue *dq = di->getDeviceQueue(device);
    NvFlowContextInterface *ci = di->getContextInterface(dq);
    NvFlowContext *ctx = di->getContext(dq);

    NvFlowGridDesc grid_desc = NvFlowGridDesc_default;
    grid_desc.maxLocations = POOL;
    grid_desc.maxLocationsIsosurface = 16;
    NvFlowGrid *grid = gi->createGrid(ci, ctx, op_list, ext_op_list, &grid_desc);
    NvFlowGridParamsNamed *named = pi->createGridParamsNamed("d3d12_flow_bench");
    NvFlowGridParams *params = named ? pi->mapGridParamsNamed(named) : NULL;
    if (!grid || !params) {
        printf("createGrid failed\n");
        return 1;
    }
    di->enableProfiler(ctx, NULL, report);

    for (int l = 0; l < LAYERS; l++)
        sim_ptrs[l] = &sim[l];

    // Beam: Matt firing at ~90 fps (one Flow step a frame, flow_every_frame)
    // at a wall 900 units off, for 3 s; rail: a shot every 1.5 s, three of
    // them, at 90 fps with Flow's 60 Hz stepper.
    const float dt = 1.f / 90.f;
    const int frames = is_beam ? 270 : 405;
    const int warmup = is_beam ? 45 : 0;
    vec3 beam_dir; vset(beam_dir, 1.f, 0.25f, -0.08f); vnorm(beam_dir);
    vec3 rail_dir; vset(rail_dir, 1.f, -0.2f, 0.05f); vnorm(rail_dir);
    vec3 fwd_right, fwd_up;
    make_normals(beam_dir, fwd_right, fwd_up);

    NvFlowTexture *density = NULL;
    NvFlowBuffer *table = NULL;
    NvFlowSparseLevelParams level = { 0 };
    uint64_t block_sum = 0, block_frames = 0;
    NvFlowUint max_blocks = 0;
    NvFlowUint64 version = 0;
    double sim_time = 0.0, cpu_sim = 0.0;
    LARGE_INTEGER qf, q0, q1;
    QueryPerformanceFrequency(&qf);

    for (int frame = 0; frame < frames; frame++) {
        const float t = frame * dt;
        num_boxes = num_spheres = 0;
        measuring = frame >= warmup;

        float rail_age = 0.f;
        if (is_beam) {
            // the view layer from the barrel mouth (cl_flow_heatbeam_muzzle
            // "14 -5.1 -7.8"), core from 16 units, rings from 80; the mirror
            // layer from the gun the body holds, all of it
            vec3 o7, o8, end;
            vma(view_origin, 14.f, beam_dir, o7);
            vma(o7, 5.1f, fwd_right, o7);
            vma(o7, -7.8f, fwd_up, o7);
            vma(view_origin, -1.f, beam_dir, o8);
            vma(o8, -20.f, fwd_up, o8);
            vma(view_origin, 900.f, beam_dir, end);
            heatbeam(L_HEATBEAM_VIEW, o7, end, 16.f, 80.f, t, dt);
            heatbeam(L_HEATBEAM_MIRROR, o8, end, 0.f, 0.f, t, dt);
            if (with_fire) {
                // the wall burning where it hits (flow_burn_spheres): puffs
                // of heat on the fire trail layer
                for (int p = 0; p < 3; p++) {
                    vec3 c;
                    vma(end, -8.f, beam_dir, c);
                    c[2] += 4.f * p;
                    NvFlowGridEmitterSphereParams *sp = sphere(0x900 + p, L_FIRE_TRAIL, c, 6.6f);
                    if (sp) {
                        sp->temperature = 1.5f;
                        sp->fuel = 0.f;
                        sp->smoke = 0.5f;
                        sp->coupleRateTemperature = sp->coupleRateSmoke = 5.f * COUPLE_RATE;
                        sp->coupleRateFuel = sp->coupleRateBurn = sp->coupleRateVelocity = sp->coupleRateDivergence = 0.f;
                    }
                }
            }
        }
        else {
            rail_age = fmodf(t, 1.5f);
            if (rail_age < 0.05f) {
                vec3 o;
                vma(view_origin, 8.f, rail_dir, o);
                rail_piece(o, rail_dir, 0.f, 256.f, 1, L_RAIL, CELL_RAIL);
                rail_piece(o, rail_dir, 256.f, 1500.f, 2, L_RAIL_FAR, CELL_RAIL_FAR);
            }
        }
        for (int l = 0; l < LAYERS; l++)
            layer_simulate(l, is_beam, rail_age, variant);

        version++;
        NvFlowDatabaseTypeSnapshot types[3];
        types[0] = (NvFlowDatabaseTypeSnapshot) { version, &NvFlowGridSimulateLayerParams_NvFlowReflectDataType, (NvFlowUint8 **)sim_ptrs, LAYERS };
        types[1] = (NvFlowDatabaseTypeSnapshot) { version, &NvFlowGridEmitterSphereParams_NvFlowReflectDataType, (NvFlowUint8 **)sphere_ptrs, (NvFlowUint64)num_spheres };
        types[2] = (NvFlowDatabaseTypeSnapshot) { version, &NvFlowGridEmitterBoxParams_NvFlowReflectDataType, (NvFlowUint8 **)box_ptrs, (NvFlowUint64)num_boxes };
        sim_time += dt;
        NvFlowGridParamsDescSnapshot snap = { { version, types, 3 }, sim_time, dt, NV_FLOW_FALSE };

        QueryPerformanceCounter(&q0);
        pi->commitParams(params, &snap);
        NvFlowGridParamsSnapshot *snapshot = pi->getParamsSnapshot(params, sim_time, 0);
        NvFlowGridParamsDesc pd = { 0 };
        NvFlowTextureAcquire *density_acquire = NULL;
        NvFlowBufferAcquire *table_acquire = NULL;
        NvFlowUint num = 0;
        if (pi->mapParamsDesc(params, snapshot, &pd)) {
            NvFlowGridRenderData rd = { 0 };
            gi->simulate(ctx, grid, &pd, NV_FLOW_FALSE);
            gi->getRenderData(ctx, grid, &rd);
            if (rd.densityTexture && rd.sparseBuffer && rd.sparseParams.levelCount > 0) {
                num = rd.sparseParams.levels[0].numLocations;
                if (frame == frames - 1 && num > 0) {
                    level = rd.sparseParams.levels[0];
                    density_acquire = ci->enqueueAcquireTexture(ctx, rd.densityTexture);
                    table_acquire = ci->enqueueAcquireBuffer(ctx, rd.sparseBuffer);
                }
            }
            pi->unmapParamsDesc(params, snapshot);
        }
        NvFlowUint64 flushed;
        if (di->flush(dq, &flushed, NULL, NULL)) {
            printf("frame %d: device removed\n", frame);
            return 1;
        }
        QueryPerformanceCounter(&q1);
        if (measuring) {
            cpu_sim += (double)(q1.QuadPart - q0.QuadPart) / qf.QuadPart;
            block_sum += num;
            block_frames++;
        }
        if (num > max_blocks)
            max_blocks = num;
        // one frame in flight, so Flow's readbacks land the same frame every
        // run: allocation is the same from run to run
        di->waitIdle(dq);
        if (density_acquire) {
            ci->getAcquiredTexture(ctx, density_acquire, &density);
            ci->getAcquiredBuffer(ctx, table_acquire, &table);
        }
    }
    di->waitIdle(dq);
    NvFlowUint64 flushed;
    di->flush(dq, &flushed, NULL, NULL);   // the last captures
    di->waitIdle(dq);
    di->flush(dq, &flushed, NULL, NULL);
    di->waitIdle(dq);

    printf("%s, variant \"%s\": %d frames (%d measured), blocks %.0f a frame on average, at most %u\n",
           scenario, variant, frames, frames - warmup, block_frames ? (double)block_sum / block_frames : 0.0, max_blocks);
    double gpu_total = 0.0;
    double order[MAX_LABELS][2];
    for (int k = 0; k < num_labels; k++) {
        gpu_total += labels[k].gpu;
        order[k][0] = k;
        order[k][1] = labels[k].gpu;
    }
    qsort(order, num_labels, sizeof(order[0]), by_gpu);
    const double per = block_frames ? 1000.0 / block_frames : 0.0;
    printf("Flow GPU %.3f ms a frame, Flow CPU (commit + simulate + flush) %.3f ms a frame; %llu captures\n",
           gpu_total * per, cpu_sim * per, (unsigned long long)captures);
    for (int k = 0; k < num_labels && k < 24; k++) {
        const int i = (int)order[k][0];
        if (labels[i].gpu * per < 0.001)
            break;
        printf("  %-40s %7.3f ms  (%5.1f passes a frame)\n", labels[i].label, labels[i].gpu * per,
               (double)labels[i].count / (block_frames ? block_frames : 1));
    }

    // the gas, cell by cell
    if (dump && density && table && level.numLocations > 0) {
        NvFlowD3D12ExternalTexture tex = { 0 };
        NvFlowD3D12ExternalBuffer buf = { 0 };
        get_texture(density, &tex);
        get_buffer(table, &buf);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        ID3D12Resource *rb_tex = NULL, *rb_buf = NULL;
        const uint8_t *texels = read_back(queue, (ID3D12Resource *)tex.resource, (D3D12_RESOURCE_STATES)tex.state, 1, &fp, &rb_tex);
        const uint32_t *words = read_back(queue, (ID3D12Resource *)buf.resource, (D3D12_RESOURCE_STATES)buf.state, 0, NULL, &rb_buf);
        const int bx = 1 << level.blockDimBits.x, by = 1 << level.blockDimBits.y, bz = 1 << level.blockDimBits.z;
        uint64_t cap = 1 << 20, n = 0;
        cell_t *cells = malloc(sizeof(cell_t) * cap);
        for (NvFlowUint i = 0; i < level.numLocations; i++) {
            const int32_t lx = (int32_t)words[level.locationOffset + 4 * i + 0];
            const int32_t ly = (int32_t)words[level.locationOffset + 4 * i + 1];
            const int32_t lz = (int32_t)words[level.locationOffset + 4 * i + 2];
            const int32_t lw = (int32_t)words[level.locationOffset + 4 * i + 3];
            const uint32_t alloc = words[level.allocationOffset + i];
            const int ax = (alloc & 0x3FF) * (bx + 2) + 1, ay = ((alloc >> 10) & 0x3FF) * (by + 2) + 1, az = ((alloc >> 20) & 0x3FF) * (bz + 2) + 1;
            for (int z = 0; z < bz; z++)
                for (int y = 0; y < by; y++) {
                    const uint16_t *row = (const uint16_t *)(texels + fp.Offset +
                        ((uint64_t)(az + z) * fp.Footprint.Height + (ay + y)) * fp.Footprint.RowPitch);
                    for (int x = 0; x < bx; x++) {
                        float v[4];
                        int any = 0;
                        for (int c = 0; c < 4; c++) {
                            v[c] = half_to_float(row[(ax + x) * 4 + c]);
                            any |= fabsf(v[c]) > 1e-4f;
                        }
                        if (!any)
                            continue;
                        if (n == cap) {
                            cap *= 2;
                            cells = realloc(cells, sizeof(cell_t) * cap);
                        }
                        cell_t *cl = &cells[n++];
                        cl->layer = lw & 0x00FFFFFF;
                        cl->x = lx * bx + x;
                        cl->y = ly * by + y;
                        cl->z = lz * bz + z;
                        memcpy(cl->v, v, sizeof(v));
                    }
                }
        }
        qsort(cells, n, sizeof(cell_t), cell_cmp);
        FILE *fo = fopen(dump, "wb");
        if (fo) {
            fwrite(&n, 8, 1, fo);
            fwrite(cells, sizeof(cell_t), n, fo);
            fclose(fo);
            printf("gas dumped: %llu cells with gas in %u blocks to %s\n", (unsigned long long)n, level.numLocations, dump);
        }
        free(cells);
        D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Unmap(rb_tex, 0, &none);
        ID3D12Resource_Unmap(rb_buf, 0, &none);
        ID3D12Resource_Release(rb_tex);
        ID3D12Resource_Release(rb_buf);
    }

    di->disableProfiler(ctx);
    di->waitIdle(dq);
    if (density)
        ci->destroyTexture(ctx, density);
    if (table)
        ci->destroyBuffer(ctx, table);
    gi->destroyGrid(ctx, grid);
    pi->destroyGridParamsNamed(named);
    di->flush(dq, &flushed, NULL, NULL);
    di->waitIdle(dq);
    di->destroyDevice(manager, device);
    di->destroyDeviceManager(manager);
    printf("d3d12 errors: %d; Flow errors: %d, warnings: %d\n", errors, flow_errors, flow_warnings);
    return errors || flow_errors ? 1 : 0;
}
