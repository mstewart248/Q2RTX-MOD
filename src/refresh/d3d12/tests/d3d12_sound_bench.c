// Headless timing of ray traced sound on D3D12: sound_trace.comp as the build
// translates it (baseq2/shader_d3d12/sound_trace.comp.dxil), driven the way
// d3d12/sound_trace.c drives it. For Matt's report of 2026-11-07: ray traced sound
// costs ~10 fps on vid_rtx 3 and next to nothing on Vulkan.
//
// 1. ALONE: one run as steamaudio.c asks for it (s_reflections_all 1: up to 32
//    sources sharing 8192 x 9 rays, 64 bounces, 1.5 s), GPU-timestamped on a compute
//    queue, against a closed 40 x 30 x 8 m room with crates (~30k triangles; base1
//    is 37k).
// 2. NEXT TO A FRAME: a DIRECT queue kept busy with "frames" (the same shader with
//    more rays - ray queries and groupshared atomics, like the path tracer's passes),
//    two in flight, while a second thread submits a sound run every 100 ms and waits
//    for it, as SA_SimThread does, on a COMPUTE queue made the way d3d12_sound_queue
//    0 / 1 / 2 makes it. Frames per second each way = what the sound costs a frame.
//
// FIRST RUN, 2026-11-07, RTX 5070 Ti, driver 621.14, Agility 620, HAGS on (16 compute
// queues per 3D queue): a run of 32 x 2304 rays x 64 bounces = 0.61 ms (16 bounces
// 0.20 ms, the room alone 0.33 ms). Beside 9.5 ms frames: no sound 94.6 frames/s,
// d3d12_sound_queue 0 95.1 (40 runs, 3.4 ms wall each), 1 (idle) 95.8 but the sound
// STARVED (1 run in 4.5 s), 2 94.0, no sound again 95.1. So the GPU half of ray traced
// sound costs a D3D12 frame nothing measurable.
//
// SECOND RUN, same evening, after Matt's in-game logs showed the frame's GPU time going
// 10.4-11.1 -> 11.7 ms with sound on (CPU flat at 1.5 ms; Vulkan 11.7 either way): the
// "frame" made of full-screen 4K passes (d3d12_sound_bench_frame.hlsl) and base1's real
// scene (third argument: the .bsp; 18 meshes, 37352 triangles as the game's, 17 sources
// = 16 speakers nearest the player start + the room). A run alone 0.89-0.98 ms (32 bounces
// 0.52, 16 0.27). Beside 12 ms frames: no sound 82.6 frames/s, compute queue 81.5 (-1.4%,
// run GPU 2.83 ms stretched), the frame's own queue 81.0 (-2.0%, 1.30 ms), own group 81.1
// (-1.9%), idle starved, no sound again 81.6 (-1.2%). Still not the game's 5-10% - hence
// d3d12_sound_stats and the live d3d12_sound_queue 0/1/2 in d3d12/sound_trace.c.
//
// From a vcvars64 prompt, at the repository root:
//   cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_sound_bench.c /Fe:build/tools/ /Fo:build/tools/
//   (the frame's shaders, with the Vulkan SDK's dxc - see d3d12_sound_bench_frame.hlsl)
//   build/tools/d3d12_sound_bench.exe C:/Q2RTX/ [seconds per arm, default 4] [map.bsp]
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../bindings.c"
#include "../accel.c"
#include "../nvapi_rt.c"
#include "../../vkpt/ptlas.c"
#include "../../vkpt/clusters.c"
#include "../../vkpt/shader/sound_trace_shared.h"

#define HEAP_BINDLESS   MAX_RIMAGES
#define HEAP_SETS       16384
#define SAMPLER_HEAP    2048

#define FIELD_FLOATS    (SOUND_TRACE_CHANNELS * SOUND_TRACE_BANDS * SOUND_TRACE_MAX_BINS)
#define FIELDS_SIZE     ((uint64_t)SOUND_TRACE_MAX_SOURCES * FIELD_FLOATS * sizeof(float))
#define DIFF_SIZE       ((uint64_t)SOUND_TRACE_MAX_QUERIES * SOUND_DIFFRACT_FLOATS * sizeof(float))
#define GROUPS_WANTED   32          // sound_trace.c's SND_GROUPS_WANTED
#define GPU_BUDGET      (8192 * 9)  // steamaudio.c's SA_REFL_GPU_BUDGET
#define MIN_RAYS        1024        // and SA_REFL_MIN_RAYS
#define BINS            150         // 1.5 s of 10 ms bins
#define WAIT_MS         2000        // sound_trace.c's SND_WAIT_MS

// sound_trace.comp's SoundTraceParams (std140) and SoundTri, as vkpt/sound_trace.h has them
typedef struct {
    float       listener[4];
    float       sources[SOUND_TRACE_MAX_SOURCES][4];
    float       materials[SOUND_TRACE_MAX_MATERIALS][4];
    uint32_t    counts[4];
    uint32_t    counts2[4];
    float       dists[4];
    float       shift[4];
    float       queries[SOUND_TRACE_MAX_QUERIES][4];
    uint32_t    counts3[4];
} params_t;

typedef struct {
    float       normal[3];
    uint32_t    material;
} snd_tri_t;

static ID3D12Device5 *dev;
static ID3D12DescriptorHeap *rheap, *sheap;
static ID3D12PipelineState *pso;
static D3D12_GPU_VIRTUAL_ADDRESS tlas;
static ID3D12Resource *tris_buf, *edges_buf;
static uint32_t rng_state = 12345;

static float frand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (rng_state >> 8) / 16777216.0f;
}

static void *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*size); fread(p, 1, *size, f); fclose(f);
    return p;
}

static ID3D12Resource *buffer(D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                     &IID_ID3D12Resource, (void **)&r)))
        return NULL;
    return r;
}

static ID3D12Resource *upload(const void *data, uint64_t size)
{
    ID3D12Resource *r = buffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p = NULL;
    ID3D12Resource_Map(r, 0, NULL, &p);
    memcpy(p, data, (size_t)size);
    ID3D12Resource_Unmap(r, 0, NULL);
    return r;
}

/* ---------------------------------------------------------------- the scene */

static float *verts;    // 3 per triangle, a plain list
static snd_tri_t *tris;
static int num_tris, cap_tris;

static void add_tri(const float *a, const float *b, const float *c, uint32_t material)
{
    if (num_tris == cap_tris) {
        cap_tris = cap_tris ? cap_tris * 2 : 4096;
        verts = realloc(verts, (size_t)cap_tris * 9 * sizeof(float));
        tris = realloc(tris, (size_t)cap_tris * sizeof(snd_tri_t));
    }
    float *v = verts + (size_t)num_tris * 9;
    memcpy(v, a, 12); memcpy(v + 3, b, 12); memcpy(v + 6, c, 12);
    const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    snd_tri_t *t = &tris[num_tris++];
    for (int i = 0; i < 3; i++)
        t->normal[i] = l > 0 ? n[i] / l : 0;
    t->material = material;
}

// a rectangle from `o` along `u` and `v`, cut into nu x nv quads
static void grid(const float *o, const float *u, const float *v, int nu, int nv, uint32_t material)
{
    for (int j = 0; j < nv; j++)
        for (int i = 0; i < nu; i++) {
            float p[4][3];
            for (int k = 0; k < 3; k++) {
                p[0][k] = o[k] + u[k] * i / nu + v[k] * j / nv;
                p[1][k] = o[k] + u[k] * (i + 1) / nu + v[k] * j / nv;
                p[2][k] = o[k] + u[k] * (i + 1) / nu + v[k] * (j + 1) / nv;
                p[3][k] = o[k] + u[k] * i / nu + v[k] * (j + 1) / nv;
            }
            add_tri(p[0], p[1], p[2], material);
            add_tri(p[0], p[2], p[3], material);
        }
}

static void crate(const float *lo, const float *hi, uint32_t material)
{
    const float dx[3] = { hi[0] - lo[0], 0, 0 }, dy[3] = { 0, hi[1] - lo[1], 0 }, dz[3] = { 0, 0, hi[2] - lo[2] };
    const float top[3] = { lo[0], lo[1], hi[2] }, fx[3] = { hi[0], lo[1], lo[2] }, fy[3] = { lo[0], hi[1], lo[2] };
    grid(lo, dx, dy, 1, 1, material);   grid(top, dx, dy, 1, 1, material);
    grid(lo, dx, dz, 1, 1, material);   grid(fy, dx, dz, 1, 1, material);
    grid(lo, dy, dz, 1, 1, material);   grid(fx, dy, dz, 1, 1, material);
}

// meshes: the world and each brush model, a BLAS each and an instance each, as
// steamaudio.c hands them to d3d12/sound_trace.c
#define MAX_MESHES 1024
static int mesh_first[MAX_MESHES + 1], num_meshes;
static void mesh_begin(void) { if (num_meshes < MAX_MESHES) mesh_first[num_meshes++] = num_tris; }
static void mesh_end(void) { mesh_first[num_meshes] = num_tris; }

// the listener and sources, in metres; from the map when there is one
static float listener_pos[3] = { 20, 15, 1.7f }, source_pos[SOUND_TRACE_MAX_SOURCES][3];
static int map_sources;     // 0 = scatter them round the room

/*
A Q2 map (IBSP 38) as steamaudio.c makes its scene: every face of the world and of
each brush model but sky, nodraw and alpha-tested ones (SA_SceneMaterial),
fan-triangulated, in Steam Audio's space - (x, z, -y) / 32, metres. The listener at
the first info_player_start (eye height), and the sources the target_speakers
nearest it - the map's own ambient sounds, what fills the echo slots - plus the
room's at the listener, as Matt's run had (17 sources).
*/
static bool load_bsp(const char *path, int want_sources)
{
    size_t size = 0;
    uint8_t *b = load(path, &size);
    if (!b || size < 160 || memcmp(b, "IBSP", 4) || *(int32_t *)(b + 4) != 38) {
        printf("not an IBSP 38 map: %s\n", path);
        return false;
    }
    const int32_t *lumps = (const int32_t *)(b + 8);
#define LUMP(i)     (b + lumps[(i) * 2])
#define LUMPN(i, s) (lumps[(i) * 2 + 1] / (s))
    const float *bverts = (const float *)LUMP(2);
    const uint8_t *texinfo = LUMP(5);
    const uint8_t *faces = LUMP(6);
    const uint16_t *edges = (const uint16_t *)LUMP(11);
    const int32_t *surfedges = (const int32_t *)LUMP(12);
    const uint8_t *models = LUMP(13);
    const int nummodels = LUMPN(13, 48), numtexinfo = LUMPN(5, 76);

    for (int m = 0; m < nummodels && num_meshes < MAX_MESHES; m++) {
        const int first = *(const int32_t *)(models + m * 48 + 40), count = *(const int32_t *)(models + m * 48 + 44);
        mesh_begin();
        for (int f = first; f < first + count; f++) {
            const uint8_t *face = faces + f * 20;
            const int firstedge = *(const int32_t *)(face + 4), numedges = *(const int16_t *)(face + 8);
            const int ti = *(const int16_t *)(face + 10);
            if (ti < 0 || ti >= numtexinfo || numedges < 3)
                continue;
            const int flags = *(const int32_t *)(texinfo + ti * 76 + 32);
            if (flags & (0x4 | 0x80 | 0x02000000))      // SURF_SKY, SURF_NODRAW, SURF_ALPHATEST
                continue;
            const uint32_t mat = (flags & 0x8) ? 2 : (flags & 0x30) ? 3 : (uint32_t)(f & 1);
            float p[64][3];
            const int n = min(numedges, 64);
            for (int e = 0; e < n; e++) {
                const int se = surfedges[firstedge + e];
                const float *v = bverts + 3 * (se >= 0 ? edges[se * 2] : edges[-se * 2 + 1]);
                p[e][0] = v[0] / 32; p[e][1] = v[2] / 32; p[e][2] = -v[1] / 32;
            }
            for (int e = 1; e + 1 < n; e++)
                add_tri(p[0], p[e], p[e + 1], mat);
        }
        if (num_tris == mesh_first[num_meshes - 1])
            num_meshes--;   // nothing left of it
    }
    mesh_end();

    // entities: the player start and the speakers
    const char *ent = (const char *)LUMP(0), *end = ent + lumps[1];
    float start[3] = { 0 }, speakers[512][3];
    int num_speakers = 0;
    bool have_start = false;
    while (ent < end) {
        const char *open = memchr(ent, '{', end - ent), *close = open ? memchr(open, '}', end - open) : NULL;
        if (!close)
            break;
        char block[2048];
        const size_t len = min((size_t)(close - open), sizeof(block) - 1);
        memcpy(block, open, len);
        block[len] = 0;
        float o[3];
        const char *op = strstr(block, "\"origin\"");
        const bool has_origin = op && sscanf(op + 8, " \"%f %f %f\"", &o[0], &o[1], &o[2]) == 3;
        if (has_origin && !have_start && strstr(block, "\"info_player_start\"")) {
            memcpy(start, o, sizeof(o));
            start[2] += 22;     // the eye
            have_start = true;
        } else if (has_origin && strstr(block, "\"target_speaker\"") && num_speakers < 512) {
            memcpy(speakers[num_speakers++], o, sizeof(o));
        }
        ent = close + 1;
    }
    free(b);
    if (!have_start) {
        printf("no info_player_start in %s\n", path);
        return false;
    }
    // the nearest speakers
    int used = 0;
    for (; used < want_sources - 1 && used < num_speakers; used++) {
        int best = used;
        float best_d = 1e30f;
        for (int i = used; i < num_speakers; i++) {
            const float dx = speakers[i][0] - start[0], dy = speakers[i][1] - start[1], dz = speakers[i][2] - start[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best_d)
                best_d = d, best = i;
        }
        float t[3];
        memcpy(t, speakers[used], 12); memcpy(speakers[used], speakers[best], 12); memcpy(speakers[best], t, 12);
        source_pos[used][0] = speakers[used][0] / 32;
        source_pos[used][1] = speakers[used][2] / 32;
        source_pos[used][2] = -speakers[used][1] / 32;
    }
    listener_pos[0] = start[0] / 32; listener_pos[1] = start[2] / 32; listener_pos[2] = -start[1] / 32;
    memcpy(source_pos[used], listener_pos, sizeof(listener_pos));   // the room
    map_sources = used + 1;
    printf("map: %s - %d meshes, %d triangles; listener at the player start, %d of its %d speakers + the room\n",
           path, num_meshes, num_tris, used, num_speakers);
    return true;
}

static void build_room(void)
{
    mesh_begin();
    const float W = 40, D = 30, H = 8;
    const float o[3] = { 0, 0, 0 }, ox[3] = { W, 0, 0 }, oy[3] = { 0, D, 0 }, oz[3] = { 0, 0, H };
    const float x[3] = { W, 0, 0 }, y[3] = { 0, D, 0 }, z[3] = { 0, 0, H };
    grid(o, x, y, 80, 60, 1);    // floor, metal
    grid(oz, x, y, 80, 60, 2);   // ceiling
    grid(o, x, z, 80, 16, 0);    // walls, rock
    grid(oy, x, z, 80, 16, 0);
    grid(o, y, z, 60, 16, 0);
    grid(ox, y, z, 60, 16, 0);
    for (int i = 0; i < 150; i++) {
        const float s = 0.5f + 2.5f * frand(), h = 0.5f + 3.f * frand();
        const float lo[3] = { 1 + (W - 2 - s) * frand(), 1 + (D - 2 - s) * frand(), 0 };
        const float hi[3] = { lo[0] + s, lo[1] + s, h };
        crate(lo, hi, 3);
    }
    mesh_end();
}

/* ----------------------------------------------------------------- a runner */

// one queue's worth of sound runs: its own parameters, fields and set
typedef struct {
    params_t        *params_map;
    ID3D12Resource  *params, *fields, *zeros, *diff, *readback;
    d3d12_set_t     set;
    uint32_t        groups, field_floats;
    int             num_sources;
} runner_t;

static bool runner_init(runner_t *r)
{
    memset(r, 0, sizeof(*r));
    const uint64_t psize = (sizeof(params_t) + 255) & ~255ull;
    r->params = buffer(D3D12_HEAP_TYPE_UPLOAD, psize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    r->fields = buffer(D3D12_HEAP_TYPE_DEFAULT, FIELDS_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    r->zeros = buffer(D3D12_HEAP_TYPE_DEFAULT, FIELDS_SIZE, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    r->diff = buffer(D3D12_HEAP_TYPE_DEFAULT, DIFF_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    r->readback = buffer(D3D12_HEAP_TYPE_READBACK, FIELDS_SIZE, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!r->params || !r->fields || !r->zeros || !r->diff || !r->readback)
        return false;
    D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Map(r->params, 0, &none, (void **)&r->params_map);
    if (!d3d12_set_alloc(&r->set, D3D12_SET_SOUND, "sound"))
        return false;
    d3d12_set_write_as(&r->set, 0, 0, tlas);
    d3d12_set_write_cbv(&r->set, 1, 0, ID3D12Resource_GetGPUVirtualAddress(r->params), (uint32_t)psize);
    d3d12_set_write_buffer(&r->set, 2, 0, tris_buf, 0, (uint64_t)num_tris * sizeof(snd_tri_t));
    d3d12_set_write_buffer(&r->set, 3, 0, r->fields, 0, FIELDS_SIZE);
    d3d12_set_write_buffer(&r->set, 4, 0, edges_buf, 0, SOUND_EDGE_FLOATS * sizeof(float));
    d3d12_set_write_buffer(&r->set, 5, 0, r->diff, 0, DIFF_SIZE);
    return true;
}

// vkpt_sound_pack_params for a run of n sources at `rays` each
static void runner_params(runner_t *r, int n, int rays, int bounces, uint32_t seed)
{
    params_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.listener, listener_pos, sizeof(listener_pos));
    rng_state = 777;    // the same sources every time
    for (int i = 0; i < n; i++) {
        if (map_sources) {
            // the map's, round again when it has fewer
            memcpy(p.sources[i], source_pos[i % map_sources], sizeof(source_pos[0]));
        } else {
            p.sources[i][0] = 2 + 36 * frand();
            p.sources[i][1] = 2 + 26 * frand();
            p.sources[i][2] = 0.5f + 2.5f * frand();
        }
    }
    static const float mats[4][4] = {
        { .15f, .20f, .25f, .30f },     // rock ("boot")
        { .20f, .25f, .30f, .20f },     // metal ("clank")
        { .10f, .13f, .16f, .40f },
        { .20f, .25f, .30f, .20f },
    };
    memcpy(p.materials, mats, sizeof(mats));
    const uint32_t per_thread = max(1u, (uint32_t)rays / (SOUND_TRACE_GROUP * GROUPS_WANTED));
    r->groups = ((uint32_t)rays + SOUND_TRACE_GROUP * per_thread - 1) / (SOUND_TRACE_GROUP * per_thread);
    r->field_floats = SOUND_TRACE_CHANNELS * SOUND_TRACE_BANDS * BINS;
    r->num_sources = n;
    p.counts[0] = n; p.counts[1] = rays; p.counts[2] = bounces; p.counts[3] = BINS;
    p.counts2[0] = per_thread; p.counts2[1] = seed;
    p.dists[0] = 1.5f * SOUND_TRACE_SPEED_OF_SOUND;
    p.dists[1] = 1.0f;
    p.dists[2] = 4 * 3.14159265f / rays;
    p.shift[0] = 0.37f; p.shift[1] = 0.61f;
    memcpy(r->params_map, &p, sizeof(p));
}

static void transition(ID3D12GraphicsCommandList4 *cl, ID3D12Resource *res, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER rb = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    rb.Transition.pResource = res;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = a;
    rb.Transition.StateAfter = b;
    ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &rb);
}

// snd_run's list: zero the fields, trace, copy them out (copy_out); timestamps
// around the dispatch into qh at q when qh is given
static void runner_record(ID3D12GraphicsCommandList4 *cl, runner_t *r, ID3D12QueryHeap *qh, UINT q,
                          ID3D12Resource *ts_readback, bool copy_out)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cl;
    ID3D12DescriptorHeap *heaps[2] = { rheap, sheap };
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
    const uint64_t bytes = (uint64_t)r->num_sources * r->field_floats * sizeof(float);
    ID3D12GraphicsCommandList4_CopyBufferRegion(cl, r->fields, 0, r->zeros, 0, bytes);
    transition(cl, r->fields, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12GraphicsCommandList4_SetPipelineState(cl, pso);
    d3d12_compute_begin(gl, D3D12_PL_SOUND);
    const d3d12_set_t *sets[1] = { &r->set };
    d3d12_compute_bind_sets(gl, D3D12_PL_SOUND, sets);
    if (qh)
        ID3D12GraphicsCommandList4_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, q);
    d3d12_compute_dispatch(gl, D3D12_PL_SOUND, r->groups, (uint32_t)r->num_sources, 1);
    if (qh) {
        ID3D12GraphicsCommandList4_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
        ID3D12GraphicsCommandList4_ResolveQueryData(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, q, 2, ts_readback, q * 8);
    }
    if (copy_out) {
        transition(cl, r->fields, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, r->readback, 0, r->fields, 0, bytes);
    }
}

/* ------------------------------------------------------------------- queues */

typedef struct {
    ID3D12CommandQueue          *queue;
    ID3D12CommandAllocator      *alloc;
    ID3D12GraphicsCommandList4  *cl;
    ID3D12Fence                 *fence;
    uint64_t                    value;
    HANDLE                      event;
    bool                        borrowed;   // queue is another lane's
} lane_t;

static const char *queue_note;

// mode 0 plain, 1 own scheduling group at idle, 2 own group; or `borrow`'s queue
// (its type) - the sound on the frame's own queue
static bool lane_init(lane_t *l, D3D12_COMMAND_LIST_TYPE type, int mode, ID3D12CommandQueue *borrow)
{
    memset(l, 0, sizeof(*l));
    D3D12_COMMAND_QUEUE_DESC qd = { type };
    queue_note = "plain";
    if (borrow) {
        l->queue = borrow;
        l->borrowed = true;
        queue_note = "the frame's own queue";
    } else if (mode) {
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_ALLOW_DYNAMIC_PRIORITY;
        if (FAILED(ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&l->queue))) {
            queue_note = "ALLOW_DYNAMIC_PRIORITY REFUSED - plain";
            qd.Flags = 0;
            l->queue = NULL;
        }
    }
    if (!l->queue && FAILED(ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&l->queue)))
        return false;
    if (borrow)
        ;
    else if (qd.Flags && mode == 2)
        queue_note = "own group, default priority";
    else if (qd.Flags) {
        ID3D12CommandQueue1 *q1 = NULL;
        HRESULT hr = ID3D12CommandQueue_QueryInterface(l->queue, &IID_ID3D12CommandQueue1, (void **)&q1);
        if (SUCCEEDED(hr)) {
            hr = ID3D12CommandQueue1_SetGlobalPriority(q1, D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_IDLE);
            ID3D12CommandQueue1_Release(q1);
        }
        queue_note = hr == S_OK ? "own group, idle priority" : "own group, IDLE REFUSED";
    }
    return SUCCEEDED(ID3D12Device5_CreateCommandAllocator(dev, type, &IID_ID3D12CommandAllocator, (void **)&l->alloc)) &&
           SUCCEEDED(ID3D12Device5_CreateCommandList(dev, 0, type, l->alloc, NULL, &IID_ID3D12GraphicsCommandList4,
                                                     (void **)&l->cl)) &&
           SUCCEEDED(ID3D12GraphicsCommandList4_Close(l->cl)) &&
           SUCCEEDED(ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&l->fence)) &&
           (l->event = CreateEventA(NULL, FALSE, FALSE, NULL)) != NULL;
}

static void lane_free(lane_t *l)
{
    if (l->fence && l->queue) {
        ID3D12CommandQueue_Signal(l->queue, l->fence, ++l->value);
        if (ID3D12Fence_GetCompletedValue(l->fence) < l->value) {
            ID3D12Fence_SetEventOnCompletion(l->fence, l->value, l->event);
            WaitForSingleObject(l->event, INFINITE);
        }
    }
    if (l->cl) ID3D12GraphicsCommandList4_Release(l->cl);
    if (l->alloc) ID3D12CommandAllocator_Release(l->alloc);
    if (l->fence) ID3D12Fence_Release(l->fence);
    if (l->queue && !l->borrowed) ID3D12CommandQueue_Release(l->queue);
    if (l->event) CloseHandle(l->event);
    memset(l, 0, sizeof(*l));
}

// executes the lane's list and waits up to ms; false = not done in time
static bool lane_submit_wait(lane_t *l, DWORD ms)
{
    ID3D12GraphicsCommandList4_Close(l->cl);
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)l->cl };
    ID3D12CommandQueue_ExecuteCommandLists(l->queue, 1, lists);
    ID3D12CommandQueue_Signal(l->queue, l->fence, ++l->value);
    if (ID3D12Fence_GetCompletedValue(l->fence) >= l->value)
        return true;
    ID3D12Fence_SetEventOnCompletion(l->fence, l->value, l->event);
    return WaitForSingleObject(l->event, ms) == WAIT_OBJECT_0;
}

static void lane_begin(lane_t *l)
{
    ID3D12CommandAllocator_Reset(l->alloc);
    ID3D12GraphicsCommandList4_Reset(l->cl, l->alloc, NULL);
}

/* ------------------------------------------------------------- 1. alone */

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double time_alone(runner_t *r, int n, int rays, int bounces, ID3D12QueryHeap *qh, ID3D12Resource *ts)
{
    lane_t l;
    if (!lane_init(&l, D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, NULL))
        return -1;
    UINT64 freq = 1;
    ID3D12CommandQueue_GetTimestampFrequency(l.queue, &freq);
    double ms[16];
    for (int i = 0; i < 17; i++) {
        runner_params(r, n, rays, bounces, 1000 + i);
        lane_begin(&l);
        runner_record(l.cl, r, qh, 0, ts, true);
        if (!lane_submit_wait(&l, 10000)) {
            printf("  the GPU did not finish a run in 10 s\n");
            lane_free(&l);
            return -1;
        }
        const UINT64 *t = NULL;
        D3D12_RANGE rr = { 0, 16 };
        ID3D12Resource_Map(ts, 0, &rr, (void **)&t);
        if (i > 0)  // the first warms up
            ms[i - 1] = (double)(t[1] - t[0]) * 1000.0 / (double)freq;
        ID3D12Resource_Unmap(ts, 0, NULL);
    }
    lane_free(&l);
    qsort(ms, 16, sizeof(ms[0]), cmp_double);
    return ms[8];
}

/* ------------------------------------------------- 2. next to a frame */

/*
THE FRAME (d3d12_sound_bench_frame.hlsl): full-screen 4K passes that fill the GPU as
the game's do. The first version of this bench used the sound shader itself as the
"frame" - 32 groups a source, latency-bound, never more than a third of the GPU's
warps - so the sound found room beside it and cost nothing, while in the game
(Matt's logs, 2026-11-07) the frame's own GPU time grew 0.6-1.3 ms with sound on.
*/
#define FRAME_W 3840
#define FRAME_H 2160

static ID3D12RootSignature *frame_rs;
static ID3D12PipelineState *frame_ray_pso, *frame_blur_pso;
static ID3D12Resource *image_a, *image_b;
static int frame_ray_passes = 4, frame_rays = 2, frame_blur_passes = 8, frame_taps = 25;
static ID3D12CommandQueue *frame_queue;     // the sound's, when it shares it

static ID3D12PipelineState *frame_pso(const char *path)
{
    size_t size = 0;
    void *code = load(path, &size);
    if (!code)
        return NULL;
    if (!frame_rs &&
        FAILED(ID3D12Device5_CreateRootSignature(dev, 0, code, size, &IID_ID3D12RootSignature, (void **)&frame_rs))) {
        free(code);
        return NULL;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof(pd));
    pd.pRootSignature = frame_rs;
    pd.CS.pShaderBytecode = code;
    pd.CS.BytecodeLength = size;
    ID3D12PipelineState *pso = NULL;
    ID3D12Device5_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    free(code);
    return pso;
}

static void frame_pass(ID3D12GraphicsCommandList4 *cl, ID3D12PipelineState *p, uint32_t index, uint32_t n)
{
    uint32_t c[8] = { index, n, FRAME_W, FRAME_H, 0, 0, 0, 0 };
    memcpy(&c[4], listener_pos, sizeof(listener_pos));
    D3D12_RESOURCE_BARRIER uav = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
    ID3D12GraphicsCommandList4_SetPipelineState(cl, p);
    ID3D12GraphicsCommandList4_SetComputeRoot32BitConstants(cl, 3, 8, c, 0);
    ID3D12GraphicsCommandList4_Dispatch(cl, FRAME_W / 8, FRAME_H / 8, 1);
    ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &uav);
}

// a frame: the ray passes spread among the blur passes; or only `only` (1 ray, 2 blur)
static void record_frame(ID3D12GraphicsCommandList4 *cl, ID3D12QueryHeap *qh, ID3D12Resource *ts, int only)
{
    ID3D12GraphicsCommandList4_SetComputeRootSignature(cl, frame_rs);
    ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(cl, 0, tlas);
    ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(image_a));
    ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(image_b));
    if (qh)
        ID3D12GraphicsCommandList4_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    if (only == 1)
        frame_pass(cl, frame_ray_pso, 0, frame_rays);
    else if (only == 2)
        frame_pass(cl, frame_blur_pso, 0, frame_taps);
    else {
        const int total = frame_ray_passes + frame_blur_passes;
        int r = 0;
        for (int i = 0; i < total; i++) {
            // spread: a ray pass whenever the share of them so far falls behind
            if (r < frame_ray_passes && r * total <= i * frame_ray_passes)
                frame_pass(cl, frame_ray_pso, (uint32_t)i, frame_rays), r++;
            else
                frame_pass(cl, frame_blur_pso, (uint32_t)i, frame_taps);
        }
    }
    if (qh) {
        ID3D12GraphicsCommandList4_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        ID3D12GraphicsCommandList4_ResolveQueryData(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, ts, 0);
    }
}

// median GPU ms of a frame (or of one pass, `only`)
static double time_frame(int only, ID3D12QueryHeap *qh, ID3D12Resource *ts)
{
    lane_t l;
    if (!lane_init(&l, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, NULL))
        return -1;
    UINT64 freq = 1;
    ID3D12CommandQueue_GetTimestampFrequency(l.queue, &freq);
    double ms[8];
    for (int i = 0; i < 9; i++) {
        lane_begin(&l);
        record_frame(l.cl, qh, ts, only);
        if (!lane_submit_wait(&l, 10000)) {
            lane_free(&l);
            return -1;
        }
        const UINT64 *t = NULL;
        D3D12_RANGE rr = { 0, 16 };
        ID3D12Resource_Map(ts, 0, &rr, (void **)&t);
        if (i > 0)
            ms[i - 1] = (double)(t[1] - t[0]) * 1000.0 / (double)freq;
        ID3D12Resource_Unmap(ts, 0, NULL);
    }
    lane_free(&l);
    qsort(ms, 8, sizeof(ms[0]), cmp_double);
    return ms[4];
}

typedef struct {
    runner_t        *runner;
    int             mode;       // 0 plain, 1 own group idle, 2 own group, 3 the frame's queue
    volatile LONG   stop;
    int             runs, late;
    double          wall_sum, wall_max, gpu_sum;
    const char      *note;
} sound_job_t;

static DWORD WINAPI sound_thread(void *arg)
{
    sound_job_t *j = arg;
    lane_t l;
    const bool shared = j->mode == 3;
    if (!lane_init(&l, shared ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE,
                   shared ? 0 : j->mode, shared ? frame_queue : NULL))
        return 1;
    j->note = queue_note;
    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 };
    ID3D12QueryHeap *qh = NULL;
    ID3D12Device5_CreateQueryHeap(dev, &qhd, &IID_ID3D12QueryHeap, (void **)&qh);
    ID3D12Resource *ts = buffer(D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 freq = 1;
    ID3D12CommandQueue_GetTimestampFrequency(l.queue, &freq);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // SDL's LOW
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    while (!j->stop) {
        QueryPerformanceCounter(&t0);
        lane_begin(&l);
        runner_record(l.cl, j->runner, qh, 0, ts, true);
        const bool done = lane_submit_wait(&l, WAIT_MS);
        if (!done) {
            j->late++;     // the game would give up on the GPU here
            ID3D12Fence_SetEventOnCompletion(l.fence, l.value, l.event);
            WaitForSingleObject(l.event, INFINITE);
        }
        QueryPerformanceCounter(&t1);
        const UINT64 *t = NULL;
        D3D12_RANGE rr = { 0, 16 };
        ID3D12Resource_Map(ts, 0, &rr, (void **)&t);
        j->gpu_sum += (double)(t[1] - t[0]) * 1000.0 / (double)freq;
        ID3D12Resource_Unmap(ts, 0, NULL);
        const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        j->runs++;
        j->wall_sum += ms;
        if (ms > j->wall_max)
            j->wall_max = ms;
        // s_reflections_rate 10: the next run 100 ms after this one began
        if (ms < 100)
            Sleep((DWORD)(100 - ms));
    }
    lane_free(&l);
    ID3D12QueryHeap_Release(qh);
    ID3D12Resource_Release(ts);
    return 0;
}

// frames per second on a DIRECT queue for `seconds`, two in flight, with the
// sound beside them (mode -1 = no sound)
static double frames_per_second(runner_t *sound, int mode, double seconds, sound_job_t *job)
{
    lane_t l[2];
    if (!lane_init(&l[0], D3D12_COMMAND_LIST_TYPE_DIRECT, 0, NULL) ||
        !lane_init(&l[1], D3D12_COMMAND_LIST_TYPE_DIRECT, 0, l[0].queue))
        return -1;
    ID3D12CommandQueue *q = frame_queue = l[0].queue;
    ID3D12GraphicsCommandList4 *lists[2] = { l[0].cl, l[1].cl };
    for (int i = 0; i < 2; i++) {
        lane_begin(&l[i]);
        record_frame(lists[i], NULL, NULL, 0);
        ID3D12GraphicsCommandList4_Close(lists[i]);
    }
    ID3D12Fence *fence = l[0].fence;
    HANDLE ev = l[0].event;

    memset(job, 0, sizeof(*job));
    job->runner = sound;
    job->mode = mode;
    HANDLE th = NULL;
    if (mode >= 0)
        th = CreateThread(NULL, 0, sound_thread, job, 0, NULL);

    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    uint64_t submitted = 0, start_frame = 0;
    bool warm = false;
    double elapsed = 0;
    int runs_at_start = 0;
    for (;;) {
        if (submitted >= 2 && ID3D12Fence_GetCompletedValue(fence) < submitted - 1) {
            ID3D12Fence_SetEventOnCompletion(fence, submitted - 1, ev);
            WaitForSingleObject(ev, INFINITE);
        }
        ID3D12CommandList *one[1] = { (ID3D12CommandList *)lists[submitted & 1] };
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, one);
        ID3D12CommandQueue_Signal(q, fence, ++submitted);

        QueryPerformanceCounter(&t);
        elapsed = (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (!warm && elapsed > 0.5) {   // half a second for the sound thread to settle
            warm = true;
            start_frame = ID3D12Fence_GetCompletedValue(fence);
            runs_at_start = job->runs;
            t0 = t;
            elapsed = 0;
        }
        if (warm && elapsed >= seconds)
            break;
    }
    const uint64_t done = ID3D12Fence_GetCompletedValue(fence) - start_frame;
    job->stop = 1;
    if (th) {
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    }
    (void)runs_at_start;
    l[0].value = submitted;
    lane_free(&l[1]);
    lane_free(&l[0]);
    frame_queue = NULL;
    return (double)done / elapsed;
}
/* --------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "C:/Q2RTX/";
    const double seconds = argc > 2 ? atof(argv[2]) : 4.0;
    const char *map_path = argc > 3 ? argv[3] : NULL;      // a .bsp: its scene instead of the room
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE get = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE ser =
        (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");

    // the Agility runtime, as the game has it (no debug layer: this is timing)
    ID3D12SDKConfiguration1 *cfg = NULL;
    get(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg);
    ID3D12DeviceFactory *fac = NULL;
    snprintf(path, sizeof(path), "%sD3D12\\", root);
    if (FAILED(ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, D3D12_SDK_VERSION, path, &IID_ID3D12DeviceFactory, (void **)&fac))) {
        printf("no Agility runtime at %s\n", path);
        return 1;
    }
    IDXGIFactory6 *xf = NULL; mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    DXGI_ADAPTER_DESC3 desc;
    IDXGIAdapter4_GetDesc3(ad, &desc);
    if (FAILED(ID3D12DeviceFactory_CreateDevice(fac, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev))) {
        printf("CreateDevice failed (or no ID3D12Device5)\n");
        return 1;
    }
    printf("%ls, Agility SDK %d\n", desc.Description, D3D12_SDK_VERSION);
    D3D12_FEATURE_DATA_HARDWARE_SCHEDULING_QUEUE_GROUPINGS g = { 0 };
    if (SUCCEEDED(ID3D12Device5_CheckFeatureSupport(dev, D3D12_FEATURE_HARDWARE_SCHEDULING_QUEUE_GROUPINGS, &g, sizeof(g))))
        printf("hardware scheduling queue groups: up to %u compute queues with each 3D queue\n", g.ComputeQueuesPer3DQueue);
    else
        printf("hardware scheduling queue groups: not reported by this runtime\n");

    // heaps and the binding layer, as device.c sets them up
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HEAP_BINDLESS + HEAP_SETS,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12Device5_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rheap);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; hd.NumDescriptors = SAMPLER_HEAP;
    ID3D12Device5_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&sheap);
    d3d12_heaps.device = (ID3D12Device *)dev;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rheap, &d3d12_heaps.resource_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(rheap, &d3d12_heaps.resource_gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(sheap, &d3d12_heaps.sampler_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(sheap, &d3d12_heaps.sampler_gpu);
    d3d12_heaps.resource_increment = ID3D12Device5_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d3d12_heaps.sampler_increment = ID3D12Device5_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    d3d12_heaps.resource_next = HEAP_BINDLESS;
    d3d12_heaps.resource_end = HEAP_BINDLESS + HEAP_SETS;
    d3d12_heaps.sampler_next = D3D12_NUM_GLOBAL_SAMPLERS;
    d3d12_heaps.sampler_end = SAMPLER_HEAP;
    char err[1024] = "";
    if (FAILED(d3d12_bindings_create_root_signatures((ID3D12Device *)dev, ser, err, sizeof(err)))) {
        printf("ROOT SIGNATURES FAILED: %s\n", err);
        return 1;
    }

    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\sound_trace.comp.dxil", root);
    size_t code_size = 0;
    void *code = load(path, &code_size);
    if (!code) { printf("FAIL: no %s (build the game first)\n", path); return 1; }
    pso = d3d12_create_compute_pso(D3D12_PL_SOUND, code, code_size, L"sound_trace.comp");
    free(code);
    if (!pso) { printf("FAIL: no PSO for sound_trace.comp against the sound layout\n"); return 1; }

    // the scene: a BLAS per mesh and an instance each, as d3d12/sound_trace.c builds it
    if (map_path) {
        if (!load_bsp(map_path, 17))
            return 1;
    } else {
        build_room();
        printf("scene: closed 40 x 30 x 8 m room with 150 crates, %d triangles\n", num_tris);
    }
    ID3D12Resource *vb = upload(verts, (uint64_t)num_tris * 9 * sizeof(float));
    tris_buf = buffer(D3D12_HEAP_TYPE_DEFAULT, (uint64_t)num_tris * sizeof(snd_tri_t),
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *tris_up = upload(tris, (uint64_t)num_tris * sizeof(snd_tri_t));
    edges_buf = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS fast = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    static d3d12_as_triangles_t geoms[MAX_MESHES];
    static uint64_t blas_size[MAX_MESHES];
    uint64_t pool_size = 0, scratch_size = 0;
    d3d12_as_sizes_t sz;
    for (int m = 0; m < num_meshes; m++) {
        memset(&geoms[m], 0, sizeof(geoms[m]));
        geoms[m].vertices = ID3D12Resource_GetGPUVirtualAddress(vb) + (uint64_t)mesh_first[m] * 9 * sizeof(float);
        geoms[m].num_vertices = (uint32_t)(mesh_first[m + 1] - mesh_first[m]) * 3;
        if (!d3d12_blas_sizes(dev, &geoms[m], 1, fast, &sz)) {
            printf("FAIL: no prebuild info\n");
            return 1;
        }
        blas_size[m] = sz.result;
        pool_size += (sz.result + 255) & ~255ull;
        scratch_size = max(scratch_size, sz.scratch);
    }
    if (!d3d12_tlas_sizes(dev, (uint32_t)num_meshes, fast, &sz)) {
        printf("FAIL: no prebuild info\n");
        return 1;
    }
    pool_size += (sz.result + 255) & ~255ull;
    scratch_size = max(scratch_size, sz.scratch);
    d3d12_as_pool_t pool;
    if (!d3d12_as_pool_create(&pool, (ID3D12Device *)dev, pool_size + 4096, L"sound bench AS"))
        return 1;
    static D3D12_GPU_VIRTUAL_ADDRESS blas[MAX_MESHES];
    for (int m = 0; m < num_meshes; m++)
        blas[m] = d3d12_as_pool_alloc(&pool, blas_size[m]);
    tlas = d3d12_as_pool_alloc(&pool, sz.result);
    ID3D12Resource *scratch = buffer(D3D12_HEAP_TYPE_DEFAULT, scratch_size,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    static D3D12_RAYTRACING_INSTANCE_DESC inst[MAX_MESHES];
    for (int m = 0; m < num_meshes; m++) {
        memset(&inst[m], 0, sizeof(inst[m]));
        inst[m].Transform[0][0] = inst[m].Transform[1][1] = inst[m].Transform[2][2] = 1.f;
        inst[m].InstanceID = (UINT)mesh_first[m];      // the shader's tri = this + PrimitiveIndex
        inst[m].InstanceMask = 0xff;
        inst[m].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        inst[m].AccelerationStructure = blas[m];
    }
    ID3D12Resource *instances = upload(inst, (uint64_t)num_meshes * sizeof(inst[0]));

    lane_t setup;
    if (!lane_init(&setup, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, NULL))
        return 1;
    lane_begin(&setup);
    ID3D12GraphicsCommandList4_CopyBufferRegion(setup.cl, tris_buf, 0, tris_up, 0, (uint64_t)num_tris * sizeof(snd_tri_t));
    for (int m = 0; m < num_meshes; m++) {
        d3d12_blas_build(setup.cl, &geoms[m], 1, fast, blas[m], ID3D12Resource_GetGPUVirtualAddress(scratch), 0);
        d3d12_as_barrier(setup.cl, NULL);   // the next reuses the scratch
    }
    d3d12_tlas_build(setup.cl, ID3D12Resource_GetGPUVirtualAddress(instances), (uint32_t)num_meshes, fast, tlas,
                     ID3D12Resource_GetGPUVirtualAddress(scratch));
    d3d12_as_barrier(setup.cl, &pool);
    if (!lane_submit_wait(&setup, 10000)) { printf("FAIL: setup did not finish\n"); return 1; }
    lane_free(&setup);

    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 };
    ID3D12QueryHeap *qh = NULL;
    ID3D12Device5_CreateQueryHeap(dev, &qhd, &IID_ID3D12QueryHeap, (void **)&qh);
    ID3D12Resource *ts = buffer(D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    runner_t sound, frame;
    if (!runner_init(&sound) || !runner_init(&frame)) { printf("FAIL: buffers\n"); return 1; }

    // 1. ALONE
    printf("\n1. ONE SOUND RUN ALONE (compute queue, GPU timestamps around the dispatch, median of 16)\n");
    static const struct { int n, bounces; const char *what; } cases[] = {
        { 32, 64, "every echo slot busy (s_reflections_all 1, 31 + the room)" },
        { 16, 64, "16 sources" },
        { 9,  64, "9 sources" },
        { 1,  64, "the room alone" },
        { 32, 32, "32 sources, 32 bounces" },
        { 32, 16, "32 sources, 16 bounces" },
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        const int n = cases[i].n;
        const int rays = min(8192, max(GPU_BUDGET / n, MIN_RAYS));
        const double ms = time_alone(&sound, n, rays, cases[i].bounces, qh, ts);
        printf("  %2d sources x %5d rays, %2d bounces: %7.2f ms   %s\n", n, rays, cases[i].bounces, ms, cases[i].what);
    }

    // 2. NEXT TO A FRAME of ~11 ms (Matt's D3D12 frame: 10.4-11.1 ms of GPU at 4K)
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    *strrchr(exe_dir, '\\') = 0;
    snprintf(path, sizeof(path), "%s\\sound_bench_ray.dxil", exe_dir);
    frame_ray_pso = frame_pso(path);
    snprintf(path, sizeof(path), "%s\\sound_bench_blur.dxil", exe_dir);
    frame_blur_pso = frame_pso(path);
    image_a = buffer(D3D12_HEAP_TYPE_DEFAULT, (uint64_t)FRAME_W * FRAME_H * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COMMON);
    image_b = buffer(D3D12_HEAP_TYPE_DEFAULT, (uint64_t)FRAME_W * FRAME_H * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COMMON);
    if (!frame_ray_pso || !frame_blur_pso || !image_a || !image_b) {
        printf("FAIL: no frame shaders next to the exe (sound_bench_ray.dxil / sound_bench_blur.dxil, see the header)\n");
        return 1;
    }
    const double ray_ms = time_frame(1, qh, ts), blur_ms = time_frame(2, qh, ts);
    // about 6 ms of ray passes and 5 of blur passes
    frame_ray_passes = max(1, (int)(6.0 / ray_ms + 0.5));
    frame_blur_passes = max(1, (int)(5.0 / blur_ms + 0.5));
    const double frame_ms = time_frame(0, qh, ts);
    printf("\n2. NEXT TO A FRAME: %.2f ms of GPU = %d ray passes (%.2f ms: %dx%d, %d rays a pixel) and %d blur passes\n"
           "   (%.2f ms: %d taps), a barrier after each; two frames in flight on a DIRECT queue. A sound run\n"
           "   (%d sources x %d rays, 64 bounces - Matt's base1 run) every 100 ms; %.0f s per arm.\n",
           frame_ms, frame_ray_passes, ray_ms, FRAME_W, FRAME_H, frame_rays, frame_blur_passes, blur_ms, frame_taps,
           17, 4336, seconds);
    runner_params(&sound, 17, 4336, 64, 7);

    static const struct { int mode; const char *what; } arms[] = {
        { -1, "no sound" },
        { 0,  "compute queue (now)" },
        { 3,  "the frame's queue" },
        { 2,  "own group" },
        { 1,  "own group, idle" },
        { -1, "no sound (again)" },
    };
    double base = 0;
    for (int i = 0; i < (int)(sizeof(arms) / sizeof(arms[0])); i++) {
        sound_job_t job;
        const double fps = frames_per_second(&sound, arms[i].mode, seconds, &job);
        if (i == 0)
            base = fps;
        printf("  %-20s %6.1f frames/s (%+5.1f%%)", arms[i].what, fps, base > 0 ? (fps / base - 1) * 100 : 0);
        if (arms[i].mode >= 0)
            printf("   %d runs: GPU %.2f ms, wall %.1f (max %.1f) ms", job.runs,
                   job.runs ? job.gpu_sum / job.runs : 0, job.runs ? job.wall_sum / job.runs : 0, job.wall_max);
        if (arms[i].mode >= 0 && job.late)
            printf(", %d over %d ms (the game would stop tracing on the GPU)", job.late, WAIT_MS);
        printf("\n");
    }
    return 0;
}
