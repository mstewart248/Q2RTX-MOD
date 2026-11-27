// rr_probe: feed DLSS Ray Reconstruction a synthetic "rippling water reflection" and
// measure which inputs keep it sharp. Standalone: D3D12 + NGX (ngx.c), no engine.
//
// The picture: every pixel shows a detailed reflected texture T, displaced by a
// time-varying ripple field d(p, t) (so the reflected picture wobbles every frame with a
// still camera), lit with 1-spp style noise. The true per-pixel motion of the reflected
// picture is known exactly, so each configuration can hand RR that motion (or not) in
// either motion channel, with guides describing a rough floor, a mirror, or the reflected
// surface. Output of the last frame is written beside a ground truth for comparison.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "C:/Q2RTX/src/refresh/d3d12/ngx.c"

#define RW 960
#define RH 540
#define OW (2 * RW)
#define OH (2 * RH)

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *cmd;
static ID3D12Fence *fence;
static HANDLE fence_event;
static UINT64 fence_value;

#define CHECK(hr, what) do { HRESULT _h = (hr); if (FAILED(_h)) { printf("%s failed 0x%08lx\n", what, (unsigned long)_h); exit(1); } } while (0)

static void submit_and_wait(void)
{
    CHECK(ID3D12GraphicsCommandList_Close(cmd), "Close");
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cmd };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    fence_value++;
    ID3D12CommandQueue_Signal(queue, fence, fence_value);
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
    CHECK(ID3D12CommandAllocator_Reset(alloc), "alloc reset");
    CHECK(ID3D12GraphicsCommandList_Reset(cmd, alloc, NULL), "list reset");
}

typedef struct {
    ID3D12Resource *tex, *up;
    uint8_t *map;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    int ch, w, h;
    float *cpu;     // w*h*ch
    bool first;
} tex_t;

static DXGI_FORMAT fmt_of(int ch)
{
    return ch == 1 ? DXGI_FORMAT_R32_FLOAT : ch == 2 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
}

static D3D12_RESOURCE_DESC tex_desc(int ch, int w, int h, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d = { 0 };
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt_of(ch);
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    return d;
}

static ID3D12Resource *make_buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC d = { 0 };
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r), "buffer");
    return r;
}

static tex_t make_input(int ch)
{
    tex_t t = { 0 };
    t.ch = ch; t.w = RW; t.h = RH; t.first = true;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d = tex_desc(ch, RW, RH, D3D12_RESOURCE_FLAG_NONE);
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&t.tex), "texture");
    UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(dev, &d, 0, 1, 0, &t.fp, NULL, NULL, &total);
    t.up = make_buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    CHECK(ID3D12Resource_Map(t.up, 0, NULL, (void **)&t.map), "map");
    t.cpu = calloc((size_t)RW * RH * ch, sizeof(float));
    return t;
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = { 0 };
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &br);
}

static void upload(tex_t *t)
{
    for (int y = 0; y < t->h; y++)
        memcpy(t->map + t->fp.Offset + (size_t)y * t->fp.Footprint.RowPitch, t->cpu + (size_t)y * t->w * t->ch, (size_t)t->w * t->ch * sizeof(float));
    if (!t->first)
        transition(t->tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    t->first = false;
    D3D12_TEXTURE_COPY_LOCATION dst = { t->tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION src = { t->up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    src.PlacedFootprint = t->fp;
    ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &dst, 0, 0, 0, &src, NULL);
    transition(t->tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// ---------------------------------------------------------------- the picture
static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static float rnd(uint32_t a, uint32_t b, uint32_t c) { return (hash3(a, b, c) >> 8) * (1.0f / 16777216.0f); }
static float *g_bn;       // the game blue noise layer 0, 256x256 (noise 4, 5)
static float bn_at(int x, int y, uint32_t f, uint32_t salt)
{
    float shift = (float)(((f + salt) * 0x9E3779B9u) >> 8) * (1.0f / 16777216.0f);
    float v = g_bn[(size_t)((y + (int)salt * 37) & 255) * 256 + ((x + (int)salt * 59) & 255)] + shift;
    return v - floorf(v);
}
static int g_dark;      // configs Y*: the floor's albedo a fifth as bright (its light makes up for it)
static int g_nograin;   // set 5: smooth textures, so a fine pattern in the output is an artifact

// the reflected surface: bricks with mortar, per-brick tint, fine grain; coordinates in
// render pixels. Returns albedo; *emis gets the torch flame (not lit, so not noisy).
static void scene(float x, float y, float alb[3], float emis[3])
{
    const float bw = 46.0f, bh = 19.0f;
    float row = floorf(y / bh);
    float xo = x + (((int)row & 1) ? bw * 0.5f : 0.0f);
    float col = floorf(xo / bw);
    float fx = xo - col * bw, fy = y - row * bh;
    bool mortar = fx < 2.0f || fy < 2.0f;
    float tint = rnd((uint32_t)(int)col + 4096, (uint32_t)(int)row + 4096, 1);
    float grain = rnd((uint32_t)(int)floorf(x / 1.5f) + 65536, (uint32_t)(int)floorf(y / 1.5f) + 65536, 2);
    if (g_nograin) grain = 0.5f;
    float v = mortar ? 0.08f : (0.30f + 0.35f * tint) * (0.65f + 0.7f * grain);
    alb[0] = v * 1.00f; alb[1] = v * (0.55f + 0.2f * tint); alb[2] = v * 0.35f;
    float dx = x - RW * 0.5f, dy = y - RH * 0.45f;
    float g = expf(-(dx * dx / (2 * 9.0f * 9.0f) + dy * dy / (2 * 26.0f * 26.0f)));
    emis[0] = 6.0f * g; emis[1] = 3.2f * g; emis[2] = 0.8f * g;
}

static int g_ripple;
static void ripple(float x, float y, float t, float d[2])
{
    if (!g_ripple) { d[0] = d[1] = 0; return; }
    d[0] = 5.0f * sinf(0.045f * x + 0.030f * y + 0.30f * t) + 3.0f * sinf(-0.030f * x + 0.070f * y + 0.43f * t);
    d[1] = 9.0f * sinf(0.020f * x + 0.050f * y + 0.33f * t) + 4.0f * cosf(0.080f * x - 0.040f * y + 0.50f * t);
}

// where the reflected point shown at p this frame was shown last frame, minus p
static void true_flow(float px, float py, float t, float mv[2])
{
    float d[2];
    ripple(px, py, t, d);
    float cx = px + d[0], cy = py + d[1];
    float qx = px, qy = py;
    for (int i = 0; i < 40; i++) {
        ripple(qx, qy, t - 1.0f, d);
        qx = cx - d[0]; qy = cy - d[1];
    }
    mv[0] = qx - px; mv[1] = qy - py;
}

static float halton(int i, int base)
{
    float f = 1, r = 0;
    while (i > 0) { f /= base; r += f * (i % base); i /= base; }
    return r;
}

typedef struct {
    const char *name;
    int ripple;
    int guides;         // 0 rough floor, 1 mirror (flat normal), 2 the reflected surface (its albedo, rough 1), 3 mirror with rippled normals, 4 half blend floor/mirror
    int primary_mv;     // 0 zero, 1 the reflected picture's motion
    int refl_mv;        // 0 not provided, 1 the reflected picture's motion, 2 provided as zero, 3 negated motion
    int refl_albedo;    // pInReflectedAlbedo = reflected albedo
    int noise;          // 1 exponential lighting noise
    float F;            // > 0: two layers, colour = F * reflection + (1 - F) * floor
    float floor_gain;   // light on the floor layer relative to the reflected surface
    float mv_noise;     // px of per-pixel random error added to the reflection motion
    float mv_scale;     // the reflection motion multiplied by this
} config_t;

static const config_t configs[] = {
    { "00_static_floor",            0, 0, 0, 0, 0, 1 },
    { "01_floor_noMV",              1, 0, 0, 0, 0, 1 },
    { "02_floor_reflMV",            1, 0, 0, 1, 0, 1 },
    { "03_mirror_reflMV",           1, 1, 0, 1, 0, 1 },
    { "04_mirror_noMV",             1, 1, 0, 0, 0, 1 },
    { "05_floor_primaryMV",         1, 0, 1, 0, 0, 1 },
    { "06_mirror_primaryMV",        1, 1, 1, 0, 0, 1 },
    { "07_psr_primaryMV",           1, 2, 1, 0, 0, 1 },
    { "08_psr_reflMV",              1, 2, 0, 1, 0, 1 },
    { "09_psr_noMV",                1, 2, 0, 0, 0, 1 },
    { "10_floor_reflAlb_reflMV",    1, 0, 0, 1, 1, 1 },
    { "11_mirror_reflAlb_reflMV",   1, 1, 0, 1, 1, 1 },
    { "12_mirror_reflMV_negated",   1, 1, 0, 3, 0, 1 },
    { "13_mirror_bothMV",           1, 1, 1, 1, 0, 1 },
    { "14_mirrorN_reflMV",          1, 3, 0, 1, 0, 1 },
    { "15_blend_reflMV",            1, 4, 0, 1, 0, 1 },
    { "16_floor_bothMV",            1, 0, 1, 1, 0, 1 },
    { "17_psr_bothMV",              1, 2, 1, 1, 0, 1 },
    { "18_floor_reflAlb_noMV",      1, 0, 0, 0, 1, 1 },
    { "19_static_mirror",           0, 1, 0, 0, 0, 1 },
    { "20_static_psr",              0, 2, 0, 0, 0, 1 },
    { "21_floor_noMV_clean",        1, 0, 0, 0, 0, 0 },
    { "22_floor_reflMV_clean",      1, 0, 0, 1, 0, 0 },
    { "23_floor_primaryMV_clean",   1, 0, 1, 0, 0, 0 },
    { "24_mirror_reflMV_clean",     1, 1, 0, 1, 0, 0 },
};

static const config_t configs2[] = {
    // name                         rip gd pm rm ra n   F     gain  mvn  mvs
    { "30_F50_floor_noMV",          1, 0, 0, 0, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "31_F50_floor_reflMV",        1, 0, 0, 1, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "32_F50_blend_reflMV",        1, 5, 0, 1, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "33_F50_split_reflMV",        1, 6, 0, 1, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "34_F50_blend_noMV",          1, 5, 0, 0, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "35_F50_psr_reflMV",          1, 2, 0, 1, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "36_F50_blend_primaryMV",     1, 5, 1, 0, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "37_F50_splitMirror_reflMV",  1, 7, 0, 1, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "40_F15_floor_reflMV",        1, 0, 0, 1, 0, 1, 0.15f, 1.0f, 0.0f, 1.0f },
    { "41_F15_blend_reflMV",        1, 5, 0, 1, 0, 1, 0.15f, 1.0f, 0.0f, 1.0f },
    { "42_F15_split_reflMV",        1, 6, 0, 1, 0, 1, 0.15f, 1.0f, 0.0f, 1.0f },
    { "43_F15dark_floor_reflMV",    1, 0, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "44_F15dark_blend_reflMV",    1, 5, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "45_F15dark_split_reflMV",    1, 6, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "46_F15dark_psr_reflMV",      1, 2, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "47_F15dark_floor_noMV",      1, 0, 0, 0, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "48_F15dark_blend_noMV",      1, 5, 0, 0, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "49_F15dark_lumblend_reflMV", 1, 8, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "50_F90_floor_reflMV",        1, 0, 0, 1, 0, 1, 0.90f, 1.0f, 0.0f, 1.0f },
    { "51_F90_blend_reflMV",        1, 5, 0, 1, 0, 1, 0.90f, 1.0f, 0.0f, 1.0f },
    { "60_mvnoise05_blend",         1, 5, 0, 1, 0, 1, 0.50f, 1.0f, 0.5f, 1.0f },
    { "61_mvnoise1_blend",          1, 5, 0, 1, 0, 1, 0.50f, 1.0f, 1.0f, 1.0f },
    { "62_mvnoise2_blend",          1, 5, 0, 1, 0, 1, 0.50f, 1.0f, 2.0f, 1.0f },
    { "63_mvscale07_blend",         1, 5, 0, 1, 0, 1, 0.50f, 1.0f, 0.0f, 0.7f },
    { "64_mvzero_blend",            1, 5, 0, 2, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "65_mvnoise1_floor",          1, 0, 0, 1, 0, 1, 0.50f, 1.0f, 1.0f, 1.0f },
    { "70_F50_blend_static",        0, 5, 0, 0, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "71_F50_floor_static",        0, 0, 0, 0, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
};

static const config_t configs3[] = {
    // guides 9 per-pixel noisy share, 10 box 5, 11 box 17, 12 per-pixel EMA 0.1, 13 box 9 + EMA 0.2
    { "80_F15dark_wNoisy_reflMV",   1, 9,  0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "81_F15dark_wBox5_reflMV",    1, 10, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "82_F15dark_wBox17_reflMV",   1, 11, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "83_F15dark_wEma_reflMV",     1, 12, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "84_F50_wBox17_reflMV",       1, 11, 0, 1, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "85_F15_wBox17_reflMV",       1, 11, 0, 1, 0, 1, 0.15f, 1.0f, 0.0f, 1.0f },
    { "86_F15dark_wBox17_noMV",     1, 11, 0, 0, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "87_F15dark_wBox17_mvnoise1", 1, 11, 0, 1, 0, 1, 0.15f, 0.03f, 1.0f, 1.0f },
    { "88_F15dark_wBox17_mvzero",   1, 11, 0, 2, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "89_F15dark_wBoxEma_reflMV",  1, 13, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "90_F15dark_floor_reflMV",    1, 0,  0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "91_F15dark_exact_reflMV",    1, 8,  0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "92_F50_wBoxEma_reflMV",      1, 13, 0, 1, 0, 1, 0.50f, 1.0f, 0.0f, 1.0f },
    { "93_F15_wBoxEma_reflMV",      1, 13, 0, 1, 0, 1, 0.15f, 1.0f, 0.0f, 1.0f },
    { "94_F15dark_wBoxEma_static",  0, 13, 0, 0, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "95_F15dark_floor_static",    0, 0,  0, 0, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "96_F15dark_wBox17_mvscale07",1, 11, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 0.7f },
    { "97_F15dark_wBox17_mvnoise05",1, 11, 0, 1, 0, 1, 0.15f, 0.03f, 0.5f, 1.0f },
};

static double *sat_r, *sat_t;
static float *lum_r, *lum_t, *ema_r, *ema_t, *alb_r, *alb_t;
static void build_sat(const float *src, double *sat)
{
    for (int y = 0; y <= RH; y++)
        for (int x = 0; x <= RW; x++)
            sat[(size_t)y * (RW + 1) + x] = (x == 0 || y == 0) ? 0.0
                : src[(size_t)(y - 1) * RW + (x - 1)] + sat[(size_t)(y - 1) * (RW + 1) + x]
                  + sat[(size_t)y * (RW + 1) + (x - 1)] - sat[(size_t)(y - 1) * (RW + 1) + (x - 1)];
}
static float box(const double *sat, int x, int y, int r)
{
    int x0 = x - r < 0 ? 0 : x - r, y0 = y - r < 0 ? 0 : y - r;
    int x1 = x + r + 1 > RW ? RW : x + r + 1, y1 = y + r + 1 > RH ? RH : y + r + 1;
    double v = sat[(size_t)y1 * (RW + 1) + x1] - sat[(size_t)y0 * (RW + 1) + x1] - sat[(size_t)y1 * (RW + 1) + x0] + sat[(size_t)y0 * (RW + 1) + x0];
    return (float)(v / ((double)(x1 - x0) * (y1 - y0)));
}

/* SET 5 (11-27): THE GLASS DOT LATTICE. guides 14 = the game's reflection_light_share
   (checkerboard_interleave.comp): 25 taps, every second pixel of a 9x9, robust mean.
   16 = the same window dense (the 11-25 try that stamped squares). 17 = the share from
   4x4 blocks (each block's sum of sqrt luminance less its largest sample), read with a
   tent over 2x2 blocks. 18 = 17 with the block sums carried over frames (EMA 0.15).
   8 = the exact share. noise 2 = pt_sparse 0.5 on top (half the pixels unlit, the rest
   doubled), 3 = that and fireflies (1 pixel in 500 at 200x). */
static const config_t configs5[] = {
    { "S0_exact_sparse",        0, 8,  0, 0, 0, 2, 0.15f, 0.30f, 0.0f, 1.0f },
    { "S1_stride2_sparse",      0, 14, 0, 0, 0, 2, 0.15f, 0.30f, 0.0f, 1.0f },
    { "S2_dense9_sparse",       0, 16, 0, 0, 0, 2, 0.15f, 0.30f, 0.0f, 1.0f },
    { "S3_blocks_sparse",       0, 17, 0, 0, 0, 2, 0.15f, 0.30f, 0.0f, 1.0f },
    { "S4_blocksEma_sparse",    0, 18, 0, 0, 0, 2, 0.15f, 0.30f, 0.0f, 1.0f },
    { "T0_exact_fire",          0, 8,  0, 0, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "T1_stride2_fire",        0, 14, 0, 0, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "T2_dense9_fire",         0, 16, 0, 0, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "T3_blocks_fire",         0, 17, 0, 0, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "T4_blocksEma_fire",      0, 18, 0, 0, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "U0_exact_ripple",        1, 8,  0, 1, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "U1_stride2_ripple",      1, 14, 0, 1, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "U3_blocks_ripple",       1, 17, 0, 1, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "U4_blocksEma_ripple",    1, 18, 0, 1, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
    { "V1_stride2_plain",       0, 14, 0, 0, 0, 1, 0.15f, 0.30f, 0.0f, 1.0f },
    { "V4_blocksEma_plain",     0, 18, 0, 0, 0, 1, 0.15f, 0.30f, 0.0f, 1.0f },
    { "W0_exact_bn",            0, 8,  0, 0, 0, 4, 0.15f, 0.30f, 0.0f, 1.0f },
    { "W1_stride2_bn",          0, 14, 0, 0, 0, 4, 0.15f, 0.30f, 0.0f, 1.0f },
    { "W2_floor_bn",            0, 0,  0, 0, 0, 4, 0.15f, 0.30f, 0.0f, 1.0f },
    { "X0_exact_bnSparse",      0, 8,  0, 0, 0, 5, 0.15f, 0.30f, 0.0f, 1.0f },
    { "X1_stride2_bnSparse",    0, 14, 0, 0, 0, 5, 0.15f, 0.30f, 0.0f, 1.0f },
    { "X2_floor_bnSparse",      0, 0,  0, 0, 0, 5, 0.15f, 0.30f, 0.0f, 1.0f },
    { "X4_blocksEma_bnSparse",  0, 18, 0, 0, 0, 5, 0.15f, 0.30f, 0.0f, 1.0f },
    { "Y0_exact_dark",          0, 8,  0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y1_stride2_dark",        0, 14, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y2_dense9_dark",         0, 16, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y3_blocks_dark",         0, 17, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y4_blocksEma_dark",      0, 18, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y5_floor_dark",          0, 0,  0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Z0_exact_darkGrain",     0, 8,  0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Z1_stride2_darkGrain",   0, 14, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Z4_blocksEma_darkGrain", 0, 18, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y6_stride2_darkFire",    0, 14, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y7_dense9_darkFire",     0, 16, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y8_blocks_darkFire",     0, 17, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "Y9_blocksEma_darkFire",  0, 18, 0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "YA_exact_darkFire",      0, 8,  0, 0, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "YR0_exact_darkMove",     1, 8,  0, 1, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "YR1_stride2_darkMove",   1, 14, 0, 1, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "YR3_blocks_darkMove",    1, 17, 0, 1, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "YR4_blocksEma_darkMove", 1, 18, 0, 1, 0, 5, 0.15f, 1.50f, 0.0f, 1.0f },
    { "V0_floor_fire",          0, 0,  0, 0, 0, 3, 0.15f, 0.30f, 0.0f, 1.0f },
};
#define BW ((RW + 3) / 4)
#define BH ((RH + 3) / 4)
static float *blk_r, *blk_t, *blk_er, *blk_et;

static const config_t configs4[] = {
    // refl_mv 4 = reflect_flow.comp's search, ported; 5 = the same, 3x3 median after
    { "A0_F15dark_wBox17_exactMV",  1, 11, 0, 1, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "A1_F15dark_wBox17_searchMV", 1, 11, 0, 4, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "A2_F15dark_wBox17_noMV",     1, 11, 0, 0, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "A3_F15dark_floor_searchMV",  1, 0,  0, 4, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
    { "A4_F15dark_wBox17_mvn025",   1, 11, 0, 1, 0, 1, 0.15f, 0.03f, 0.25f, 1.0f },
    { "A5_F15dark_wBox17_mvn010",   1, 11, 0, 1, 0, 1, 0.15f, 0.03f, 0.10f, 1.0f },
    { "A6_F15dark_wBox17_searchMed",1, 11, 0, 5, 0, 1, 0.15f, 0.03f, 0.0f, 1.0f },
};

// ---- reflect_flow.comp's measure(), ported: positions are the reflected points (2D here)
static float *pos_now, *pos_prev;
static int pt(const float *a, int x, int y, float o[2])
{
    if (x < 0 || y < 0 || x >= RW || y >= RH) return 0;
    o[0] = a[((size_t)y * RW + x) * 2]; o[1] = a[((size_t)y * RW + x) * 2 + 1];
    return 1;
}
static float d2(const float a[2], const float b[2]) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]); }
static float step2_now(int x, int y, int ax, int ay, const float here[2])
{
    float a[2], b[2];
    float da = pt(pos_now, x + ax, y + ay, a) ? d2(a, here) : 1e30f;
    float db = pt(pos_now, x - ax, y - ay, b) ? d2(b, here) : 1e30f;
    return da < db ? da : db;
}
static int prev_slope(int x, int y, int ax, int ay, const float p0[2], float slope[2])
{
    float pa[2], pb[2];
    int a_ok = pt(pos_prev, x + ax, y + ay, pa), b_ok = pt(pos_prev, x - ax, y - ay, pb);
    slope[0] = slope[1] = 0;
    if (!a_ok && !b_ok) return 0;
    float da = a_ok ? d2(pa, p0) : 1e30f, db = b_ok ? d2(pb, p0) : 1e30f;
    if (a_ok && b_ok && da <= 4 * db && db <= 4 * da) { slope[0] = 0.5f * (pa[0] - pb[0]); slope[1] = 0.5f * (pa[1] - pb[1]); }
    else if (da <= db) { slope[0] = pa[0] - p0[0]; slope[1] = pa[1] - p0[1]; }
    else { slope[0] = p0[0] - pb[0]; slope[1] = p0[1] - pb[1]; }
    return 1;
}
static void ring_offset(int r, int i, int o[2])
{
    if (r == 0) { o[0] = o[1] = 0; return; }
    int side = i / (2 * r), t = i - side * 2 * r;
    if (side == 0) { o[0] = -r + t; o[1] = -r; }
    else if (side == 1) { o[0] = r; o[1] = -r + t; }
    else if (side == 2) { o[0] = r - t; o[1] = r; }
    else { o[0] = -r; o[1] = r - t; }
}
static int measure(int x, int y, const float jn[2], const float jp[2], int radius, float mv[2])
{
    float here[2];
    pt(pos_now, x, y, here);
    const float *target = here;     // a still world: the point was where it is
    float sx = step2_now(x, y, 1, 0, here), sy = step2_now(x, y, 0, 1, here);
    if (sx >= 1e29f && sy >= 1e29f) return 0;
    if (sx >= 1e29f) sx = sy;
    if (sy >= 1e29f) sy = sx;
    float tol2 = 0.5f * (sx + sy);
    int gx = x + (int)floorf(jn[0] - jp[0] + 0.5f), gy = y + (int)floorf(jn[1] - jp[1] + 0.5f);
    int bx = gx, by = gy; float best = 1e30f;
    for (int r = 0; r <= radius && best >= 1e30f; r++) {
        int n = r == 0 ? 1 : 8 * r, stride = r <= 8 ? 1 : 2;
        for (int i = 0; i < n; i += stride) {
            int o[2]; ring_offset(r, i, o);
            float q[2];
            if (!pt(pos_prev, gx + o[0], gy + o[1], q)) continue;
            float d = d2(q, target);
            if (d < tol2 && d < best) { best = d; bx = gx + o[0]; by = gy + o[1]; }
        }
    }
    if (best >= 1e30f) return 0;
    int cx = bx, cy = by;
    for (int oy = -1; oy <= 1; oy++)
        for (int ox = -1; ox <= 1; ox++) {
            float q[2];
            if (!pt(pos_prev, cx + ox, cy + oy, q)) continue;
            float d = d2(q, target);
            if (d < best) { best = d; bx = cx + ox; by = cy + oy; }
        }
    float p0[2], jx[2], jy[2], fr[2] = { 0, 0 };
    pt(pos_prev, bx, by, p0);
    if (prev_slope(bx, by, 1, 0, p0, jx) && prev_slope(bx, by, 0, 1, p0, jy)) {
        float xx = jx[0] * jx[0] + jx[1] * jx[1], yy = jy[0] * jy[0] + jy[1] * jy[1], xy = jx[0] * jy[0] + jx[1] * jy[1];
        float det = xx * yy - xy * xy;
        float rx = target[0] - p0[0], ry = target[1] - p0[1];
        float djx = jx[0] * rx + jx[1] * ry, djy = jy[0] * rx + jy[1] * ry;
        if (det > 0.01f * xx * yy) {
            fr[0] = (yy * djx - xy * djy) / det; fr[1] = (xx * djy - xy * djx) / det;
            for (int k = 0; k < 2; k++) fr[k] = fr[k] < -0.5f ? -0.5f : fr[k] > 0.5f ? 0.5f : fr[k];
        }
    }
    mv[0] = (bx - x) + fr[0] + jp[0] - jn[0];
    mv[1] = (by - y) + fr[1] + jp[1] - jn[1];
    return 1;
}
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

// the floor under the water: static tiles with grout and grain
static void floor_scene(float x, float y, float alb[3])
{
    const float ts = 31.0f;
    float cx = floorf(x / ts), cy = floorf(y / ts);
    float fx = x - cx * ts, fy = y - cy * ts;
    bool grout = fx < 1.5f || fy < 1.5f;
    float tint = rnd((uint32_t)(int)cx + 9000, (uint32_t)(int)cy + 9000, 5);
    float grain = rnd((uint32_t)(int)floorf(x / 1.5f) + 30000, (uint32_t)(int)floorf(y / 1.5f) + 30000, 6);
    if (g_nograin) grain = 0.5f;
    float v = grout ? 0.06f : (0.25f + 0.3f * tint) * (0.65f + 0.7f * grain);
    if (g_dark) v *= 0.2f;
    alb[0] = v * 0.55f; alb[1] = v * 0.9f; alb[2] = v * 0.8f;
}

static void write_raw(const char *dir, const char *name, const float *rgb, int w, int h, int ch)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%dx%dx%d.f32", dir, name, w, h, ch);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return; }
    fwrite(rgb, sizeof(float), (size_t)w * h * ch, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    int frames = argc > 2 ? atoi(argv[2]) : 48;
    float jitter_sign = argc > 3 ? (float)atof(argv[3]) : -1.0f;
    const char *only = argc > 4 ? argv[4] : NULL;

    IDXGIFactory6 *xf = NULL;
    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void **)&xf), "factory");
    IDXGIAdapter4 *ad = NULL;
    CHECK(IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad), "adapter");
    CHECK(D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev), "device");
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue), "queue");
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc), "allocator");
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cmd), "list");
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence), "fence");
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    NVSDK_NGX_Parameter *params = NULL;
    CreateDirectoryA("DLSSTemp", NULL);
    NVSDK_NGX_Result r = d3d12_ngx_init(dev, "FFA5FAF5-2329-44AB-A423-3D9B3B177C88", "rr_probe", L"C:\\Q2RTX", L"DLSSTemp/", NULL, &params);
    if (NVSDK_NGX_FAILED(r)) { printf("NGX init failed 0x%08x\n", (unsigned)r); return 1; }
    int rr_ok = 0;
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rr_ok);
    printf("Ray Reconstruction %s\n", rr_ok ? "available" : "NOT available");
    if (!rr_ok) return 1;

    tex_t color = make_input(4), depth = make_input(1), motion = make_input(2), dalb = make_input(4), salb = make_input(4);
    tex_t normal = make_input(4), rough = make_input(1), hitd = make_input(1), rmotion = make_input(2), ralb = make_input(4);

    ID3D12Resource *output = NULL, *readback = NULL;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp;
    {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC d = tex_desc(4, OW, OH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&output), "output");
        UINT64 total = 0;
        ID3D12Device_GetCopyableFootprints(dev, &d, 0, 1, 0, &ofp, NULL, NULL, &total);
        readback = make_buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    float *out_cpu = malloc((size_t)OW * OH * 4 * sizeof(float));
    float *gt = malloc((size_t)OW * OH * 3 * sizeof(float));

    const int set2 = argc > 5 && atoi(argv[5]) == 2;
    const int set3 = argc > 5 && atoi(argv[5]) == 3;
    const int set4 = argc > 5 && atoi(argv[5]) == 4;
    const int set5 = argc > 5 && atoi(argv[5]) == 5;
    g_nograin = set5;
    { FILE *bf = fopen("bn0.f32", "rb"); if (bf) { g_bn = malloc(65536 * sizeof(float)); if (fread(g_bn, sizeof(float), 65536, bf) != 65536) g_bn = NULL; fclose(bf); } printf("blue noise %s" "\n", g_bn ? "loaded" : "NOT loaded"); }
    blk_r = calloc((size_t)BW * BH, sizeof(float)); blk_t = calloc((size_t)BW * BH, sizeof(float));
    blk_er = calloc((size_t)BW * BH, sizeof(float)); blk_et = calloc((size_t)BW * BH, sizeof(float));
    pos_now = calloc((size_t)RW * RH * 2, sizeof(float)); pos_prev = calloc((size_t)RW * RH * 2, sizeof(float));
    const config_t *table = set5 ? configs5 : set4 ? configs4 : set3 ? configs3 : set2 ? configs2 : configs;
    const int ntable = set5 ? (int)(sizeof(configs5) / sizeof(configs5[0])) : set4 ? (int)(sizeof(configs4) / sizeof(configs4[0])) : set3 ? (int)(sizeof(configs3) / sizeof(configs3[0])) : set2 ? (int)(sizeof(configs2) / sizeof(configs2[0])) : (int)(sizeof(configs) / sizeof(configs[0]));
    sat_r = malloc(sizeof(double) * (RW + 1) * (RH + 1)); sat_t = malloc(sizeof(double) * (RW + 1) * (RH + 1));
    lum_r = malloc(sizeof(float) * RW * RH); lum_t = malloc(sizeof(float) * RW * RH);
    ema_r = malloc(sizeof(float) * RW * RH); ema_t = malloc(sizeof(float) * RW * RH);
    alb_r = malloc(sizeof(float) * RW * RH * 3); alb_t = malloc(sizeof(float) * RW * RH * 3);
    for (int c = 0; c < ntable; c++) {
        const config_t *cf = &table[c];
        if (only && !strstr(cf->name, only))
            continue;
        g_ripple = cf->ripple;
        g_dark = cf->name[0] == 'Y' || cf->name[0] == 'Z';
        g_nograin = set5 && cf->name[0] != 'Z';

        NVSDK_NGX_Handle *feature = NULL;
        {
            const int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
            NVSDK_NGX_DLSSD_Create_Params rr = { 0 };
            rr.InWidth = RW; rr.InHeight = RH; rr.InTargetWidth = OW; rr.InTargetHeight = OH;
            rr.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf;
            rr.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
            rr.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
            rr.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
            rr.InFeatureCreateFlags = flags;
            NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F);
            NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Denoise, 1);
            NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
            NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Unpacked);
            NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Use_HW_Depth, NVSDK_NGX_DLSS_Depth_Type_Linear);
            r = d3d12_ngx_create(cmd, params, true, NULL, &rr, &feature);
            if (NVSDK_NGX_FAILED(r)) { printf("RR create failed 0x%08x\n", (unsigned)r); return 1; }
            submit_and_wait();
        }

        for (int f = 0; f < frames; f++) {
            const float t = (float)f;
            const float jx = halton(f % 32 + 1, 2) - 0.5f, jy = halton(f % 32 + 1, 3) - 0.5f;
            int y;
            #pragma omp parallel for schedule(dynamic, 8)
            for (y = 0; y < RH; y++) {
                for (int x = 0; x < RW; x++) {
                    const size_t i = (size_t)y * RW + x;
                    const float sx = x + 0.5f + jx, sy = y + 0.5f + jy;
                    float d[2], alb[3], emis[3];
                    ripple(sx, sy, t, d);
                    scene(sx + d[0], sy + d[1], alb, emis);
                    pos_now[i * 2] = sx + d[0]; pos_now[i * 2 + 1] = sy + d[1];
                    float L = 1.0f;
                    float sparse_gain = 1.0f, fire = 1.0f;
                    if (cf->noise) {
                        float u = rnd((uint32_t)x, (uint32_t)y, (uint32_t)f * 7919u + 13u);
                        L = -logf(1.0f - u * 0.999f);
                    }
                    if (cf->noise >= 4 && g_bn) {
                        // the game: blue noise for the light sample, and (5) sparse_pixel_active's mask
                        L = -logf(1.0f - bn_at(x, y, (uint32_t)f, 1) * 0.999f);
                        if (cf->noise == 5)
                            sparse_gain = bn_at(x + 97, y + 131, (uint32_t)f, 0) < 0.5f ? 2.0f : 0.0f;
                        if (strstr(cf->name, "Fire") && rnd((uint32_t)x + 333u, (uint32_t)y + 444u, (uint32_t)f * 941u + 7u) < 0.002f)
                            fire = 200.0f;
                    } else
                    if (cf->noise >= 2)
                        sparse_gain = rnd((uint32_t)x + 111u, (uint32_t)y + 222u, (uint32_t)f * 613u + 3u) < 0.5f ? 2.0f : 0.0f;
                    if (cf->noise >= 3 && rnd((uint32_t)x + 333u, (uint32_t)y + 444u, (uint32_t)f * 941u + 7u) < 0.002f)
                        fire = 200.0f;
                    L *= sparse_gain * fire;
                    float falb[3] = { 0, 0, 0 };
                    if (cf->F > 0.0f) {
                        floor_scene(sx, sy, falb);
                        float u2 = rnd((uint32_t)x + 5000u, (uint32_t)y + 7000u, (uint32_t)f * 104729u + 5u);
                        float L2 = cf->noise ? -logf(1.0f - u2 * 0.999f) : 1.0f;
                        if (cf->noise >= 4 && g_bn) L2 = -logf(1.0f - bn_at(x, y, (uint32_t)f, 2) * 0.999f);
                        L2 *= sparse_gain;
                        float cr[3], ct[3];
                        for (int k = 0; k < 3; k++) {
                            cr[k] = cf->F * (alb[k] * L + emis[k]);
                            ct[k] = (1.0f - cf->F) * falb[k] * L2 * cf->floor_gain;
                            color.cpu[i * 4 + k] = cr[k] + ct[k];
                            alb_r[i * 3 + k] = alb[k]; alb_t[i * 3 + k] = falb[k];
                        }
                        lum_r[i] = 0.25f * cr[0] + 0.5f * cr[1] + 0.25f * cr[2];
                        lum_t[i] = 0.25f * ct[0] + 0.5f * ct[1] + 0.25f * ct[2];
                    } else
                    for (int k = 0; k < 3; k++)
                        color.cpu[i * 4 + k] = alb[k] * L + emis[k];
                    color.cpu[i * 4 + 3] = 1.0f;

                    float mv[2] = { 0, 0 };
                    if (cf->ripple && (cf->primary_mv || cf->refl_mv == 1 || cf->refl_mv == 3))
                        true_flow(x + 0.5f, y + 0.5f, t, mv);
                    motion.cpu[i * 2 + 0] = cf->primary_mv ? mv[0] / RW : 0.0f;
                    motion.cpu[i * 2 + 1] = cf->primary_mv ? mv[1] / RH : 0.0f;
                    float rs = cf->refl_mv == 1 ? 1.0f : cf->refl_mv == 3 ? -1.0f : 0.0f;
                    float ex = 0, ey = 0;
                    if (cf->F > 0.0f) {
                        rs *= cf->mv_scale;
                        if (cf->mv_noise > 0.0f) {
                            ex = (rnd((uint32_t)x, (uint32_t)y, (uint32_t)f * 31u + 1000003u) * 2.0f - 1.0f) * cf->mv_noise * 1.73f;
                            ey = (rnd((uint32_t)x, (uint32_t)y, (uint32_t)f * 37u + 2000003u) * 2.0f - 1.0f) * cf->mv_noise * 1.73f;
                        }
                    }
                    rmotion.cpu[i * 2 + 0] = (rs * mv[0] + ex) / RW;
                    rmotion.cpu[i * 2 + 1] = (rs * mv[1] + ey) / RH;

                    // the floor under the water: a dim static checker
                    const float fl = ((((x / 24) + (y / 24)) & 1) ? 0.10f : 0.20f);
                    float da[3], sa[3], n[3] = { 0, 0, 1 }, ro;
                    switch (cf->guides) {
                    default:
                    case 0: da[0] = da[1] = da[2] = fl; sa[0] = sa[1] = sa[2] = 0.04f; ro = 1.0f; break;
                    case 3: n[0] = -d[0] * 0.03f; n[1] = -d[1] * 0.03f; {
                                float l = sqrtf(n[0] * n[0] + n[1] * n[1] + 1.0f); n[0] /= l; n[1] /= l; n[2] = 1.0f / l; }
                            // fall through
                    case 1: da[0] = da[1] = da[2] = 0.0f; sa[0] = sa[1] = sa[2] = 0.5f; ro = 0.0f; break;
                    case 2: da[0] = alb[0]; da[1] = alb[1]; da[2] = alb[2]; sa[0] = sa[1] = sa[2] = 0.04f; ro = 1.0f; break;
                    case 4: da[0] = da[1] = da[2] = fl * 0.5f; sa[0] = sa[1] = sa[2] = 0.27f; ro = 0.5f; break;
                    }
                    if (cf->F > 0.0f) {
                        const float F = cf->F;
                        // light-weighted blend: what share of the pixel the reflection really is
                        const float wl = F / (F + (1.0f - F) * cf->floor_gain);
                        for (int k = 0; k < 3; k++) {
                            switch (cf->guides) {
                            default:
                            case 0: da[k] = falb[k]; sa[k] = 0.04f; ro = 1.0f; break;
                            case 2: da[k] = alb[k]; sa[k] = 0.04f; ro = 1.0f; break;
                            case 5: da[k] = F * alb[k] + (1.0f - F) * falb[k]; sa[k] = 0.04f; ro = 1.0f; break;
                            case 6: da[k] = (1.0f - F) * falb[k]; sa[k] = F * alb[k]; ro = 1.0f; break;
                            case 7: da[k] = (1.0f - F) * falb[k]; sa[k] = F * alb[k]; ro = 0.0f; break;
                            case 8: da[k] = wl * alb[k] + (1.0f - wl) * falb[k]; sa[k] = 0.04f; ro = 1.0f; break;
                            }
                        }
                    }
                    for (int k = 0; k < 3; k++) {
                        dalb.cpu[i * 4 + k] = da[k];
                        salb.cpu[i * 4 + k] = sa[k];
                        normal.cpu[i * 4 + k] = n[k];
                        ralb.cpu[i * 4 + k] = alb[k];
                    }
                    dalb.cpu[i * 4 + 3] = salb.cpu[i * 4 + 3] = normal.cpu[i * 4 + 3] = ralb.cpu[i * 4 + 3] = 1.0f;
                    rough.cpu[i] = ro;
                    depth.cpu[i] = 400.0f;
                    hitd.cpu[i] = 300.0f;
                }
            }
            if (cf->refl_mv >= 4) {
                static float pjx, pjy;
                const float jn[2] = { jx, jy }, jp[2] = { pjx, pjy };
                double esum = 0, e2 = 0; int found = 0, bad = 0;
                float *tmp = malloc(sizeof(float) * RW * RH * 2);
                for (int yy = 0; yy < RH; yy++)
                    for (int xx = 0; xx < RW; xx++) {
                        const size_t i = (size_t)yy * RW + xx;
                        float mv[2] = { 0, 0 };
                        int ok = f > 0 && measure(xx, yy, jn, jp, 16, mv);
                        tmp[i * 2] = mv[0]; tmp[i * 2 + 1] = mv[1];
                        if (f == frames - 1 && xx > 40 && yy > 40 && xx < RW - 40 && yy < RH - 40) {
                            float tf[2]; true_flow(xx + 0.5f, yy + 0.5f, t, tf);
                            float e = sqrtf((mv[0] - tf[0]) * (mv[0] - tf[0]) + (mv[1] - tf[1]) * (mv[1] - tf[1]));
                            esum += e; e2 += e * e; found += ok; bad += e > 0.5f;
                        }
                    }
                for (int yy = 0; yy < RH; yy++)
                    for (int xx = 0; xx < RW; xx++) {
                        const size_t i = (size_t)yy * RW + xx;
                        float mx = tmp[i * 2], my = tmp[i * 2 + 1];
                        if (cf->refl_mv == 5 && xx > 0 && yy > 0 && xx < RW - 1 && yy < RH - 1) {
                            float ax[9], ay[9]; int n = 0;
                            for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
                                ax[n] = tmp[(i + oy * RW + ox) * 2]; ay[n++] = tmp[(i + oy * RW + ox) * 2 + 1]; }
                            qsort(ax, 9, sizeof(float), cmpf); qsort(ay, 9, sizeof(float), cmpf);
                            mx = ax[4]; my = ay[4];
                        }
                        rmotion.cpu[i * 2] = mx / RW; rmotion.cpu[i * 2 + 1] = my / RH;
                    }
                free(tmp);
                if (f == frames - 1) {
                    const double n = (double)(RW - 81) * (RH - 81);
                    printf("  search MV vs true: mean err %.3f px, rms %.3f px, found %.1f%%, off by > 0.5 px %.1f%%\n", esum / n, sqrt(e2 / n), 100.0 * found / n, 100.0 * bad / n);
                }
                pjx = jx; pjy = jy;
            }
            memcpy(pos_prev, pos_now, sizeof(float) * RW * RH * 2);
            if (cf->F > 0.0f && cf->guides >= 9) {
                const int g = cf->guides;
                if (g == 12 || g == 13) {
                    const float a = f == 0 ? 1.0f : (g == 12 ? 0.1f : 0.2f);
                    if (g == 13) {
                        build_sat(lum_r, sat_r); build_sat(lum_t, sat_t);
                    }
                    for (int yy = 0; yy < RH; yy++)
                        for (int xx = 0; xx < RW; xx++) {
                            const size_t i = (size_t)yy * RW + xx;
                            const float r = g == 13 ? box(sat_r, xx, yy, 4) : lum_r[i];
                            const float tt = g == 13 ? box(sat_t, xx, yy, 4) : lum_t[i];
                            ema_r[i] += (r - ema_r[i]) * a; ema_t[i] += (tt - ema_t[i]) * a;
                        }
                } else if (g == 10 || g == 11) {
                    build_sat(lum_r, sat_r); build_sat(lum_t, sat_t);
                } else if (g == 17 || g == 18) {
                    for (int by = 0; by < BH; by++)
                        for (int bx = 0; bx < BW; bx++) {
                            float sr = 0, st = 0, mr = 0, mt = 0;
                            for (int oy = 0; oy < 4; oy++)
                                for (int ox = 0; ox < 4; ox++) {
                                    const int px = bx * 4 + ox, py = by * 4 + oy;
                                    if (px >= RW || py >= RH) continue;
                                    const float a = sqrtf(lum_r[(size_t)py * RW + px]), b = sqrtf(lum_t[(size_t)py * RW + px]);
                                    sr += a; st += b;
                                    if (a > mr) mr = a;
                                    if (b > mt) mt = b;
                                }
                            sr -= mr; st -= mt;
                            const size_t bi = (size_t)by * BW + bx;
                            const float a = (g == 18 && f > 0) ? 0.15f : 1.0f;
                            blk_er[bi] += (sr - blk_er[bi]) * a; blk_et[bi] += (st - blk_et[bi]) * a;
                            blk_r[bi] = blk_er[bi]; blk_t[bi] = blk_et[bi];
                        }
                }
                for (int yy = 0; yy < RH; yy++)
                    for (int xx = 0; xx < RW; xx++) {
                        const size_t i = (size_t)yy * RW + xx;
                        float r, tt;
                        if (g == 14 || g == 16) {
                            // reflection_light_share: the square of the summed square roots
                            const int st = g == 14 ? 2 : 1, n = g == 14 ? 2 : 4;
                            float sr = 0, stt = 0;
                            for (int oy = -n; oy <= n; oy++)
                                for (int ox = -n; ox <= n; ox++) {
                                    const int qx = xx + ox * st, qy = yy + oy * st;
                                    if (qx < 0 || qy < 0 || qx >= RW || qy >= RH) continue;
                                    sr += sqrtf(lum_r[(size_t)qy * RW + qx]); stt += sqrtf(lum_t[(size_t)qy * RW + qx]);
                                }
                            r = sr * sr; tt = stt * stt;
                        } else if (g == 17 || g == 18) {
                            // a tent over the 2x2 blocks round the pixel
                            float fx = (xx + 0.5f) / 4.0f - 0.5f, fy = (yy + 0.5f) / 4.0f - 0.5f;
                            int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                            float wx = fx - x0, wy = fy - y0, sr = 0, stt = 0;
                            for (int k = 0; k < 4; k++) {
                                int bx = x0 + (k & 1), by = y0 + (k >> 1);
                                bx = bx < 0 ? 0 : bx >= BW ? BW - 1 : bx; by = by < 0 ? 0 : by >= BH ? BH - 1 : by;
                                const float w2 = ((k & 1) ? wx : 1 - wx) * ((k >> 1) ? wy : 1 - wy);
                                sr += w2 * blk_r[(size_t)by * BW + bx]; stt += w2 * blk_t[(size_t)by * BW + bx];
                            }
                            r = sr * sr; tt = stt * stt;
                        } else
                        if (g == 9) { r = lum_r[i]; tt = lum_t[i]; }
                        else if (g == 10) { r = box(sat_r, xx, yy, 2); tt = box(sat_t, xx, yy, 2); }
                        else if (g == 11) { r = box(sat_r, xx, yy, 8); tt = box(sat_t, xx, yy, 8); }
                        else { r = ema_r[i]; tt = ema_t[i]; }
                        const float w = (r + tt) > 1e-6f ? r / (r + tt) : cf->F;
                        for (int k = 0; k < 3; k++)
                            dalb.cpu[i * 4 + k] = w * alb_r[i * 3 + k] + (1.0f - w) * alb_t[i * 3 + k];
                    }
            }
            upload(&color); upload(&depth); upload(&motion); upload(&dalb); upload(&salb);
            upload(&normal); upload(&rough); upload(&hitd); upload(&rmotion); upload(&ralb);

            d3d12_ngx_eval_t e = { 0 };
            e.rr = true;
            e.color = color.tex; e.output = output; e.depth = depth.tex; e.motion = motion.tex;
            e.diffuse_albedo = dalb.tex; e.specular_albedo = salb.tex; e.normals = normal.tex; e.roughness = rough.tex;
            e.specular_hit_distance = hitd.tex;
            e.reflection_motion = cf->refl_mv ? rmotion.tex : NULL;
            e.reflected_albedo = cf->refl_albedo ? ralb.tex : NULL;
            e.width = RW; e.height = RH;
            e.jitter_x = jx * jitter_sign; e.jitter_y = jy * jitter_sign;
            e.reset = f == 0;
            r = d3d12_ngx_evaluate(cmd, feature, params, &e);
            if (NVSDK_NGX_FAILED(r)) { printf("evaluate failed 0x%08x\n", (unsigned)r); return 1; }

            if (f == frames - 1) {
                transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION src = { output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                dst.PlacedFootprint = ofp;
                ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &dst, 0, 0, 0, &src, NULL);
                transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            submit_and_wait();
        }

        uint8_t *p = NULL;
        CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&p), "readback map");
        for (int y = 0; y < OH; y++)
            memcpy(out_cpu + (size_t)y * OW * 4, p + ofp.Offset + (size_t)y * ofp.Footprint.RowPitch, (size_t)OW * 4 * sizeof(float));
        ID3D12Resource_Unmap(readback, 0, NULL);
        write_raw(outdir, cf->name, out_cpu, OW, OH, 4);

        // ground truth for this config's last frame, 4x4 per output pixel
        const float tl = (float)(frames - 1);
        int gy;
        #pragma omp parallel for schedule(dynamic, 8)
        for (gy = 0; gy < OH; gy++)
            for (int x = 0, y = gy; x < OW; x++) {
                float acc[3] = { 0, 0, 0 };
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        const float px = (x + (sx + 0.5f) * 0.25f) * 0.5f, py = (y + (sy + 0.5f) * 0.25f) * 0.5f;
                        float d[2], alb[3], emis[3];
                        ripple(px, py, tl, d);
                        scene(px + d[0], py + d[1], alb, emis);
                        if (cf->F > 0.0f) {
                            float falb[3];
                            floor_scene(px, py, falb);
                            for (int k = 0; k < 3; k++) acc[k] += cf->F * (alb[k] + emis[k]) + (1.0f - cf->F) * falb[k] * cf->floor_gain;
                        } else
                        for (int k = 0; k < 3; k++) acc[k] += alb[k] + emis[k];
                    }
                for (int k = 0; k < 3; k++) gt[((size_t)y * OW + x) * 3 + k] = acc[k] / 16.0f;
            }
        char gname[128];
        if (cf->F > 0.0f) snprintf(gname, sizeof(gname), "gt_%s", cf->name);
        else
        snprintf(gname, sizeof(gname), "gt_%s", cf->ripple ? "ripple" : "static");
        write_raw(outdir, gname, gt, OW, OH, 3);
        if (c == 1 || (only && cf->ripple))
            write_raw(outdir, "input_last", color.cpu, RW, RH, 4);

        d3d12_ngx_release(feature);
        printf("%s done\n", cf->name);
        fflush(stdout);
    }

    d3d12_ngx_shutdown(dev, params);
    return 0;
}
