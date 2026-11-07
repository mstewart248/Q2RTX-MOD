// Headless check of the partitioned TLAS (vkpt/ptlas.c's core, accel.c's D3D12
// routes) with the same scene and shader as d3d12_accel_test.c, over five
// frames of one persistent PTLAS:
//   frame 1  build from nothing: A (index 0, global partition) at x -2,
//            B (index 1, partition 0) at x +2 z 3          -> 2 writes
//   frame 2  update in place: A moved to z 1, B dropped    -> 1 write + 1 removal
//   frame 3  nothing changed                               -> no build recorded
//   frame 4  the same, A volatile (first_volatile 0)       -> 1 write, in place
//   frame 5  ptlas_static_changed()                        -> 1 write, from nothing
// and each frame's rays are checked against the analytic answer.
//
// Routes: by default NVAPI on the retail runtime (D3D12/, any RTX GPU whose
// driver reports it); `preview` as the second argument takes D3D12's own RT
// tier 2.0 on the preview runtime (D3D12-preview/, which needs Windows
// Developer Mode). Without the route it says why and exits 2 (skipped).
// `nogbv` as the third argument leaves GPU-based validation off.
//
// Build (after d3d12_accel_test.c's two shader commands), from the repository root:
//   cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_ptlas_test.c /Fe:build/tools/ /Fo:build/tools/
//   build/tools/d3d12_ptlas_test.exe C:/Q2RTX/ [nvapi|preview] [nogbv]   (0 = pass, 1 = fail, 2 = skipped)
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bindings.c"
#include "../accel.c"
#include "../nvapi_rt.c"
#include "../../vkpt/ptlas.c"
#include "../../vkpt/clusters.c"

#define HEAP_BINDLESS   MAX_RIMAGES
#define HEAP_SETS       16384
#define SAMPLER_HEAP    2048
#define GRID            16
#define FRAMES          5

static int errors;
static ID3D12Device5 *dev;

static void __stdcall msg(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id, LPCSTR d, void *ctx)
{
    (void)c; (void)ctx;
    if (s <= D3D12_MESSAGE_SEVERITY_WARNING) {
        if (s <= D3D12_MESSAGE_SEVERITY_ERROR)
            errors++;
        if (errors < 40)
            printf("  [%s %d] %s\n", s <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning", (int)id, d);
    }
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
    if (data)
        memcpy(p, data, (size_t)size);
    else
        memset(p, 0, (size_t)size);
    ID3D12Resource_Unmap(r, 0, NULL);
    return r;
}

static uint32_t expected(int frame, int x, int y)
{
    const float ox = -4.f + (x + .5f) * .5f, oy = -4.f + (y + .5f) * .5f;
    if (oy > -1.f && oy < 1.f) {
        if (ox > -3.f && ox < -1.f) return (7u << 24) | (frame == 0 ? 1000u : 900u);   // A (custom index 7 << 24, rayquery_test.comp): z 0, then z 1
        if (ox > 1.f && ox < 3.f && frame == 0) return (9u << 24) | 700u;               // B: z 3, then gone
    }
    return 0xffffffffu;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "C:/Q2RTX/";
    const bool want_preview = argc > 2 && !strcmp(argv[2], "preview");
    const bool gbv = !(argc > 3 && !strcmp(argv[3], "nogbv"));
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE get = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE ser =
        (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");

    // the retail runtime as device.c's factory loads it, or the preview one as
    // it loads it for d3d12_agility_preview 1
    ID3D12SDKConfiguration1 *cfg = NULL;
    get(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg);
    ID3D12DeviceFactory *f = NULL;
#ifdef D3D12_PREVIEW_SDK_VERSION
    const UINT version = want_preview ? D3D12_PREVIEW_SDK_VERSION : D3D12_SDK_VERSION;
#else
    if (want_preview) {
        printf("skipped: built without the preview Agility SDK headers\n");
        return 2;
    }
    const UINT version = D3D12_SDK_VERSION;
#endif
    snprintf(path, sizeof(path), "%s%s\\", root, want_preview ? "D3D12-preview" : "D3D12");
    HRESULT hr = ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, version, path, &IID_ID3D12DeviceFactory, (void **)&f);
    if (FAILED(hr)) {
        printf("skipped: no runtime %u at %s (0x%08lx)%s\n", version, path, (unsigned long)hr,
               hr == (HRESULT)0x887E0003 ? " - turn on Windows Developer Mode (Settings > System > For developers)" : "");
        return 2;
    }
    if (want_preview) {
        hr = ID3D12DeviceFactory_EnableExperimentalFeatures(f, 1, &D3D12ExperimentalShaderModels, NULL, NULL);
        if (FAILED(hr))
            printf("note: experimental shader models refused (0x%08lx)\n", (unsigned long)hr);
    }
    ID3D12Debug1 *dbg = NULL;
    if (SUCCEEDED(ID3D12DeviceFactory_GetConfigurationInterface(f, &CLSID_D3D12Debug, &IID_ID3D12Debug1, (void **)&dbg))) {
        ID3D12Debug1_EnableDebugLayer(dbg);
        if (gbv)
            ID3D12Debug1_SetEnableGPUBasedValidation(dbg, TRUE);
    }
    IDXGIFactory6 *xf = NULL; mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    if (FAILED(ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev))) {
        printf("FAIL: CreateDevice on runtime %u\n", version);
        return 1;
    }
    const d3d12_ptlas_route_t route = d3d12_ptlas_route(dev);
    if (route != (want_preview ? D3D12_PTLAS_PREVIEW : D3D12_PTLAS_NVAPI)) {
        printf("skipped: no %s partitioned TLAS here (route: %s)\n", want_preview ? "RT tier 2.0" : "NVAPI",
               d3d12_ptlas_route_name(route));
        return 2;
    }
    printf("route: %s, runtime %u, GPU-based validation %s\n", d3d12_ptlas_route_name(route), version, gbv ? "on" : "off");
    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device5_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        DWORD cookie; ID3D12InfoQueue1_RegisterMessageCallback(iq, msg, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    }

    // heaps + binding layer
    ID3D12DescriptorHeap *rheap = NULL, *sheap = NULL;
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
    for (int i = 0; i < HEAP_BINDLESS; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = d3d12_heaps.resource_cpu;
        h.ptr += (SIZE_T)i * d3d12_heaps.resource_increment;
        ID3D12Device5_CreateShaderResourceView(dev, NULL, &sv, h);
    }
    char err[1024] = "";
    if (FAILED(d3d12_bindings_create_root_signatures((ID3D12Device *)dev, ser, err, sizeof(err)))) {
        printf("FAIL: root signatures: %s\n", err);
        return 1;
    }

    // the two classic BLASes (a PTLAS instances ordinary BLASes)
    static const float list_vertices[] = { -1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0 };
    static const float indexed_vertices[] = { -1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0 };
    static const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
    ID3D12Resource *vb_list = upload(list_vertices, sizeof(list_vertices));
    ID3D12Resource *vb_indexed = upload(indexed_vertices, sizeof(indexed_vertices));
    ID3D12Resource *ib = upload(indices, sizeof(indices));
    const d3d12_as_triangles_t geom_list = { ID3D12Resource_GetGPUVirtualAddress(vb_list), 6, 0, 0 };
    const d3d12_as_triangles_t geom_indexed = { ID3D12Resource_GetGPUVirtualAddress(vb_indexed), 4,
                                                ID3D12Resource_GetGPUVirtualAddress(ib), 6 };
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS trace = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    d3d12_as_sizes_t s_list, s_indexed, s_ptlas;
    d3d12_blas_sizes(dev, &geom_list, 1, trace, &s_list);
    d3d12_blas_sizes(dev, &geom_indexed, 1, trace, &s_indexed);

    ptlas_t ptlas;
    ptlas_init(&ptlas, 64, 4);
    if (!d3d12_ptlas_sizes(dev, route, &ptlas, true, &s_ptlas)) {
        printf("FAIL: no PTLAS prebuild info\n");
        return 1;
    }
    printf("PTLAS for 64 instances: %llu B, scratch %llu B\n", (unsigned long long)s_ptlas.result,
           (unsigned long long)s_ptlas.scratch);

    d3d12_as_pool_t pool;
    d3d12_as_pool_create(&pool, (ID3D12Device *)dev, s_list.result + s_indexed.result + s_ptlas.result + 4096, L"PTLAS test pool");
    const D3D12_GPU_VIRTUAL_ADDRESS blas_list = d3d12_as_pool_alloc(&pool, s_list.result);
    const D3D12_GPU_VIRTUAL_ADDRESS blas_indexed = d3d12_as_pool_alloc(&pool, s_indexed.result);
    const D3D12_GPU_VIRTUAL_ADDRESS ptlas_va = d3d12_as_pool_alloc(&pool, s_ptlas.result);
    uint64_t scratch_size = s_list.scratch + s_indexed.scratch + s_ptlas.scratch + 4 * D3D12_AS_ALIGNMENT;
    ID3D12Resource *scratch = buffer(D3D12_HEAP_TYPE_DEFAULT, scratch_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_COMMON);
    const D3D12_GPU_VIRTUAL_ADDRESS scratch_va = ID3D12Resource_GetGPUVirtualAddress(scratch);
    const uint64_t s1 = (s_list.scratch + 255) & ~255ull, s2 = s1 + ((s_indexed.scratch + 255) & ~255ull);

    // the staging for three frames' ops, and three answer buffers
    const uint64_t stage_each = ptlas_stage_size(64);
    ID3D12Resource *stage = upload(NULL, stage_each * FRAMES);
    uint8_t *stage_cpu = NULL;
    ID3D12Resource_Map(stage, 0, NULL, (void **)&stage_cpu);
    const D3D12_GPU_VIRTUAL_ADDRESS stage_gpu = ID3D12Resource_GetGPUVirtualAddress(stage);

    ID3D12Resource *hits[FRAMES], *readback[FRAMES];
    d3d12_set_t rt_set, ubo_set[FRAMES], tex_set, vb_set, tile_set;
    d3d12_set_alloc(&rt_set, D3D12_SET_RT, "rt");
    d3d12_set_alloc(&tex_set, D3D12_SET_TEXTURES, "textures");
    d3d12_set_alloc(&vb_set, D3D12_SET_VERTEX_BUFFER, "vertex buffer");
    d3d12_set_alloc(&tile_set, D3D12_SET_PT_TILES, "tiles");
    tex_set.external[0] = 0;
    for (int s = 0; s < D3D12_NUM_GLOBAL_SAMPLERS; s++) {
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(s, &sd);
        d3d12_set_write_sampler(&tex_set, D3D12_BINDING_GLOBAL_SAMPLERS, s, &sd);
    }
    d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, ptlas_va);
    for (int fr = 0; fr < FRAMES; fr++) {
        hits[fr] = buffer(D3D12_HEAP_TYPE_DEFAULT, GRID * GRID * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COMMON);
        readback[fr] = buffer(D3D12_HEAP_TYPE_READBACK, GRID * GRID * 4, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST);
        d3d12_set_alloc(&ubo_set[fr], D3D12_SET_UBO, "ubo");
        d3d12_set_write_buffer(&ubo_set[fr], GLOBAL_INSTANCE_BUFFER_BINDING_IDX, 0, hits[fr], 0, GRID * GRID * 4);
    }

    snprintf(path, sizeof(path), "%sbuild\\tools\\rayquery_test.dxil", root);
    size_t code_size = 0;
    void *code = load(path, &code_size);
    if (!code) { printf("FAIL: no %s - see d3d12_accel_test.c for the two shader commands\n", path); return 1; }
    ID3D12PipelineState *pso = d3d12_create_compute_pso(D3D12_PL_PATH_TRACER, code, code_size, L"rayquery test");
    free(code);
    if (!pso) { printf("FAIL: PSO\n"); return 1; }

    ID3D12CommandQueue *q = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12Device5_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca);
    ID3D12GraphicsCommandList4 *cl = NULL;
    ID3D12Device5_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList4, (void **)&cl);

    d3d12_blas_build(cl, &geom_list, 1, trace, blas_list, scratch_va, 0);
    d3d12_blas_build(cl, &geom_indexed, 1, trace, blas_indexed, scratch_va + s1, 0);
    d3d12_as_barrier(cl, NULL);

    // the three frames' instance lists
    D3D12_RAYTRACING_INSTANCE_DESC a = { 0 }, b;
    a.Transform[0][0] = a.Transform[1][1] = a.Transform[2][2] = 1.f;
    a.Transform[0][3] = -2.f;
    a.InstanceID = 7; a.InstanceMask = 0xff; a.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
    a.AccelerationStructure = blas_list;
    b = a;
    b.Transform[0][3] = 2.f; b.Transform[2][3] = 3.f;
    b.InstanceID = 9; b.AccelerationStructure = blas_indexed;
    D3D12_RAYTRACING_INSTANCE_DESC list[FRAMES][2] = { { a, b }, { a }, { a }, { a }, { a } };
    for (int fr = 1; fr < FRAMES; fr++)
        list[fr][0].Transform[2][3] = 1.f;
    const uint32_t counts[FRAMES] = { 2, 1, 1, 1, 1 };
    const uint32_t volatile_from[FRAMES] = { 2, 1, 1, 0, 1 };
    const uint32_t want_ops[FRAMES] = { 2, 2, 0, 1, 1 };
    const int want_update[FRAMES] = { 0, 1, -1, 1, 0 };     // -1: no build recorded
    uint32_t got_ops[FRAMES];
    int got_update[FRAMES];

    ID3D12DescriptorHeap *heaps[2] = { rheap, sheap };
    for (int fr = 0; fr < FRAMES; fr++) {
        if (fr == 4)
            ptlas_static_changed();
        const bool build = ptlas_diff(&ptlas, list[fr], counts[fr], 1, volatile_from[fr], d3d12_ptlas_flags(true));
        got_ops[fr] = ptlas.num_writes;
        got_update[fr] = build ? ptlas_diff_is_update(&ptlas) : -1;
        if (build && !d3d12_ptlas_record(cl, route, &ptlas, true, stage_cpu + stage_each * fr,
                                         stage_gpu + stage_each * fr, ptlas_va, scratch_va + s2)) {
            printf("FAIL: frame %d: the route refused the build\n", fr + 1);
            return 1;
        }
        d3d12_as_barrier(cl, NULL);

        ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
        ID3D12GraphicsCommandList4_SetPipelineState(cl, pso);
        d3d12_compute_begin((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER);
        const d3d12_set_t *sets[5] = { &rt_set, &ubo_set[fr], &tex_set, &vb_set, &tile_set };
        d3d12_compute_bind_sets((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, sets);
        uint32_t push[D3D12_PUSH_CONSTANT_DWORDS] = { 0 };
        d3d12_compute_push_constants((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, push, sizeof(push));
        d3d12_compute_dispatch((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, GRID / 8, GRID / 8, 1);
        d3d12_as_barrier(cl, NULL);     // the trace has read the PTLAS before the next update writes it

        D3D12_RESOURCE_BARRIER tb = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        tb.Transition.pResource = hits[fr];
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &tb);
        ID3D12GraphicsCommandList4_CopyResource(cl, readback[fr], hits[fr]);
    }
    ID3D12GraphicsCommandList4_Close(cl);
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);

    ID3D12Fence *fence = NULL;
    ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ID3D12CommandQueue_Signal(q, fence, 1);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("FAIL: the GPU did not finish in 10 s\n"); return 1; }
    hr = ID3D12Device5_GetDeviceRemovedReason(dev);
    if (FAILED(hr)) { printf("FAIL: device removed 0x%08lx\n", (unsigned long)hr); return 1; }

    int wrong_total = 0;
    for (int fr = 0; fr < FRAMES; fr++) {
        const uint32_t *got = NULL;
        D3D12_RANGE range = { 0, GRID * GRID * 4 };
        ID3D12Resource_Map(readback[fr], 0, &range, (void **)&got);
        int wrong = 0;
        for (int y = 0; y < GRID; y++)
            for (int x = 0; x < GRID; x++)
                if (got[y * GRID + x] != expected(fr, x, y) && wrong++ < 5)
                    printf("  frame %d ray %2d,%2d: got %08x, want %08x\n", fr + 1, x, y, got[y * GRID + x], expected(fr, x, y));
        static const char *kinds[3] = { "no build", "from nothing", "in place" };
        printf("frame %d: %u ops (want %u), %s (want %s), %d rays wrong\n", fr + 1, got_ops[fr], want_ops[fr],
               kinds[got_update[fr] + 1], kinds[want_update[fr] + 1], wrong);
        for (int y = GRID / 2 + 1; y >= GRID / 2 - 2; y--) {
            printf("  ");
            for (int x = 0; x < GRID; x++) {
                const uint32_t g = got[y * GRID + x];
                putchar(g == 0xffffffffu ? '.' : (g >> 24) == 7 ? 'A' : (g >> 24) == 9 ? 'B' : '?');
            }
            putchar('\n');
        }
        ID3D12Resource_Unmap(readback[fr], 0, NULL);
        wrong_total += wrong + (got_ops[fr] != want_ops[fr]) + (got_update[fr] != want_update[fr]);
    }
    printf("\n%s: %d mismatches, %d validation errors\n", wrong_total || errors ? "FAIL" : "PASS", wrong_total, errors);
    ptlas_shutdown(&ptlas);
    d3d12_as_pool_destroy(&pool);
    d3d12_bindings_destroy_root_signatures();
    return (wrong_total == 0 && errors == 0) ? 0 : 1;
}
