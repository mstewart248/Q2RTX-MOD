// Headless check of the clustered per-frame BLASes (pt_clusters: vkpt/clusters.c's
// plan and staging, accel.c's NVAPI path) against the classic ones they replace.
// One positions buffer holds three unindexed runs, as instance_geometry's output
// does, every triangle a small one over one cell of a 16x16 grid:
//   A  256 triangles at z 0 (custom index 7), every cell, in a shuffled order -
//      four clusters, the last of ONE triangle
//   B   90 triangles at z 2 (custom index 9), 90 cells - two clusters (85 + 5)
//   C    3 triangles at z 4 (custom index 11), never clustered (as the
//      explosions are not), so one plan mixes both kinds
// It is planned, built and traced twice through d3d12_dynamic_blas_plan/_build,
// each pass with its own memory as a frame slot has: whole, then with A and B
// clustered. cluster_test.comp reports which triangle of which run each ray hit
// by the hit shaders' rule (geometry index * PT_CLUSTER_TRIANGLES + primitive
// index), checked against the analytic answer. The pipeline is made after
// NvAPI_D3D12_SetCreatePipelineStateOptions(ENABLE_CLUSTER_SUPPORT), as
// world.c makes its own under pt_clusters 1.
// A third pass is the first one's memory again, as a frame slot's next frame:
// B is PERSISTENT (accel.h, the blood's pt_blood_blas_reuse) and stands unbuilt
// - its vertices are zeroed first, so only the BLAS pass 1 built can still give
// its hits. Then a plan-only check: a persistent geometry that changed is built.
// Each pass is executed and waited for, as a frame slot's fence would.
//
// From a vcvars64 prompt, at the repository root:
//   C:/VulkanSDK/1.4.350.0/Bin/glslang.exe -V --target-env vulkan1.2 -S comp src/refresh/d3d12/tests/cluster_test.comp -o build/tools/cluster_test.spv
//   build/tools/spv2dxil.exe --dxc C:/VulkanSDK/1.4.350.0/Bin --sm 6_6 build/tools/cluster_test.spv build/tools/cluster_test.dxil
//   cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_cluster_test.c /Fe:build/tools/ /Fo:build/tools/
//   build/tools/d3d12_cluster_test.exe C:/Q2RTX/ [nogbv]   (0 = pass, 1 = fail, 2 = skipped: no NVAPI clusters)
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
#define CELLS           (GRID * GRID)
#define PASSES          3

enum { RUN_A, RUN_B, RUN_C, NUM_RUNS };
static const uint32_t run_prims[NUM_RUNS] = { 256, 90, 3 };
static const uint32_t run_custom[NUM_RUNS] = { 7, 9, 11 };
static const float run_z[NUM_RUNS] = { 0.f, 2.f, 4.f };

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

// which cell triangle t of a run covers (-1: none). A: a permutation of every
// cell; B: 90 distinct cells; C: three in a row
static int cell_of(int run, uint32_t t)
{
    switch (run) {
    case RUN_A: return (int)((t * 37u) % CELLS);
    case RUN_B: return (int)((t * 13u + 5u) % CELLS);
    default:    return (int)(100u + t);
    }
}

// what the ray down through cell c should report: the highest run there
static uint32_t expected(int c)
{
    for (int run = NUM_RUNS - 1; run >= 0; run--)
        for (uint32_t t = 0; t < run_prims[run]; t++)
            if (cell_of(run, t) == c)
                return (run_custom[run] << 24) | ((uint32_t)(10.f - run_z[run] + .5f) << 16) | t;
    return 0xffffffffu;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "C:/Q2RTX/";
    const bool gbv = !(argc > 2 && !strcmp(argv[2], "nogbv"));
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE get = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE ser =
        (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");

    // the retail runtime as device.c's factory loads it
    ID3D12SDKConfiguration1 *cfg = NULL;
    get(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg);
    ID3D12DeviceFactory *f = NULL;
    snprintf(path, sizeof(path), "%sD3D12\\", root);
    HRESULT hr = ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, D3D12_SDK_VERSION, path, &IID_ID3D12DeviceFactory,
                                                             (void **)&f);
    if (FAILED(hr)) {
        printf("skipped: no runtime %u at %s (0x%08lx)\n", D3D12_SDK_VERSION, path, (unsigned long)hr);
        return 2;
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
        printf("FAIL: CreateDevice\n");
        return 1;
    }
    if (!d3d12_nvapi_clusters_supported((ID3D12Device *)dev)) {
        printf("skipped: this GPU or driver reports no NVAPI cluster operations\n");
        return 2;
    }
    if (!d3d12_nvapi_cluster_pipelines(dev, true)) {
        printf("FAIL: NvAPI_D3D12_SetCreatePipelineStateOptions(ENABLE_CLUSTER_SUPPORT) refused\n");
        return 1;
    }
    printf("NVAPI cluster operations, runtime %u, GPU-based validation %s\n", D3D12_SDK_VERSION, gbv ? "on" : "off");
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

    // the positions: A, then B, then C, 9 floats a triangle
    uint32_t first[NUM_RUNS], total = 0;
    for (int run = 0; run < NUM_RUNS; run++) {
        first[run] = total;
        total += run_prims[run];
    }
    float *pos = calloc((size_t)total * 9, sizeof(float));
    for (int run = 0; run < NUM_RUNS; run++)
        for (uint32_t t = 0; t < run_prims[run]; t++) {
            const int c = cell_of(run, t);
            const float cx = -4.f + (c % GRID + .5f) * .5f, cy = -4.f + (c / GRID + .5f) * .5f;
            float *v = pos + (size_t)(first[run] + t) * 9;
            v[0] = cx - .2f; v[1] = cy - .2f; v[2] = run_z[run];
            v[3] = cx + .2f; v[4] = cy - .2f; v[5] = run_z[run];
            v[6] = cx;       v[7] = cy + .2f; v[8] = run_z[run];
        }
    ID3D12Resource *positions = upload(pos, (uint64_t)total * 9 * sizeof(float));
    free(pos);
    const D3D12_GPU_VIRTUAL_ADDRESS positions_va = ID3D12Resource_GetGPUVirtualAddress(positions);

    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS trace = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    d3d12_as_sizes_t s_tlas;
    d3d12_tlas_sizes(dev, NUM_RUNS, trace, &s_tlas);
    d3d12_as_pool_t tlas_pool;
    d3d12_as_pool_create(&tlas_pool, (ID3D12Device *)dev, PASSES * (s_tlas.result + D3D12_AS_ALIGNMENT), L"cluster test TLASes");
    ID3D12Resource *tlas_scratch = buffer(D3D12_HEAP_TYPE_DEFAULT, PASSES * ((s_tlas.scratch + 255) & ~255ull),
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    snprintf(path, sizeof(path), "%sbuild\\tools\\cluster_test.dxil", root);
    size_t code_size = 0;
    void *code = load(path, &code_size);
    if (!code) { printf("FAIL: no %s - see the two shader commands at the top\n", path); return 1; }
    ID3D12PipelineState *pso = d3d12_create_compute_pso(D3D12_PL_PATH_TRACER, code, code_size, L"cluster test");
    free(code);
    if (!pso) { printf("FAIL: PSO\n"); return 1; }

    d3d12_set_t tex_set, vb_set, tile_set;
    d3d12_set_alloc(&tex_set, D3D12_SET_TEXTURES, "textures");
    d3d12_set_alloc(&vb_set, D3D12_SET_VERTEX_BUFFER, "vertex buffer");
    d3d12_set_alloc(&tile_set, D3D12_SET_PT_TILES, "tiles");
    tex_set.external[0] = 0;
    for (int s = 0; s < D3D12_NUM_GLOBAL_SAMPLERS; s++) {
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(s, &sd);
        d3d12_set_write_sampler(&tex_set, D3D12_BINDING_GLOBAL_SAMPLERS, s, &sd);
    }

    ID3D12CommandQueue *q = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12Device5_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca);
    ID3D12GraphicsCommandList4 *cl = NULL;
    ID3D12Device5_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList4, (void **)&cl);
    ID3D12DescriptorHeap *heaps[2] = { rheap, sheap };

    // pass 3 runs on pass 1's memory again
    d3d12_dynamic_as_t mem[PASSES];
    memset(mem, 0, sizeof(mem));
    ID3D12Resource *hits[PASSES], *readback[PASSES], *instances[PASSES];
    d3d12_set_t rt_set[PASSES], ubo_set[PASSES];
    ID3D12Fence *fence = NULL;
    ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    D3D12_GPU_VIRTUAL_ADDRESS persist_address = 0;
    int plan_wrong = 0;
    for (int pass = 0; pass < PASSES; pass++) {
        const bool clustered = pass == 1, reuse = pass == 2;
        d3d12_dynamic_as_t *m = &mem[reuse ? 0 : pass];
        if (reuse) {
            // B's vertices gone: a rebuild of it would hit nothing
            float *v = NULL;
            ID3D12Resource_Map(positions, 0, NULL, (void **)&v);
            memset(v + (size_t)first[RUN_B] * 9, 0, (size_t)run_prims[RUN_B] * 9 * sizeof(float));
            ID3D12Resource_Unmap(positions, 0, NULL);
        }
        d3d12_dynamic_blas_t blases[NUM_RUNS];
        memset(blases, 0, sizeof(blases));
        for (int run = 0; run < NUM_RUNS; run++) {
            blases[run].geometry = d3d12_triangle_run(positions_va, first[run], run_prims[run]);
            blases[run].clustered = clustered && run != RUN_C;
        }
        blases[RUN_B].persistent = !clustered;
        blases[RUN_B].reuse = reuse;
        if (!d3d12_dynamic_blas_plan(dev, m, blases, NUM_RUNS, trace)) {
            printf("FAIL: pass %d: the plan found no memory\n", pass + 1);
            return 1;
        }
        printf("pass %d (%s):", pass + 1, clustered ? "A, B clustered" : reuse ? "pass 1's memory, B reused" : "whole, B persistent");
        for (int run = 0; run < NUM_RUNS; run++) {
            printf(" %c %s %llu B%s", 'A' + run, blases[run].reused ? "reused" : blases[run].clustered ? "clusters" : "whole",
                   (unsigned long long)blases[run].result_size, run < NUM_RUNS - 1 ? "," : "\n");
            if (blases[run].clustered != (clustered && run != RUN_C) || !blases[run].address) {
                printf("  FAIL: %c planned %s at %llx\n", 'A' + run, blases[run].clustered ? "clustered" : "whole",
                       (unsigned long long)blases[run].address);
                plan_wrong++;
            }
        }
        if (pass == 0)
            persist_address = blases[RUN_B].address;
        if (blases[RUN_B].reused != reuse || (reuse && blases[RUN_B].address != persist_address)) {
            printf("  FAIL: B %s at %llx (pass 1 built it at %llx)\n", blases[RUN_B].reused ? "reused" : "built",
                   (unsigned long long)blases[RUN_B].address, (unsigned long long)persist_address);
            plan_wrong++;
        }
        if (clustered)
            printf("  plan: %u clusters over %u BLASes (largest %u), %u triangles; CLAS memory %llu B, staging %llu B\n",
                   m->cluster_plan.num_clusters, m->cluster_plan.num_blases,
                   m->cluster_plan.max_clusters_per_blas, m->cluster_plan.num_triangles,
                   (unsigned long long)m->clas.size, (unsigned long long)m->cluster_layout.size);
        if (!d3d12_dynamic_blas_build(cl, m, blases, NUM_RUNS, trace)) {
            printf("FAIL: pass %d: NVAPI refused the clustered builds\n", pass + 1);
            return 1;
        }

        // the TLAS over the three, identity transforms
        D3D12_RAYTRACING_INSTANCE_DESC list[NUM_RUNS];
        memset(list, 0, sizeof(list));
        for (int run = 0; run < NUM_RUNS; run++) {
            list[run].Transform[0][0] = list[run].Transform[1][1] = list[run].Transform[2][2] = 1.f;
            list[run].InstanceID = run_custom[run];
            list[run].InstanceMask = 0xff;
            list[run].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            list[run].AccelerationStructure = blases[run].address;
        }
        instances[pass] = upload(list, sizeof(list));
        const D3D12_GPU_VIRTUAL_ADDRESS tlas = d3d12_as_pool_alloc(&tlas_pool, s_tlas.result);
        d3d12_tlas_build(cl, ID3D12Resource_GetGPUVirtualAddress(instances[pass]), NUM_RUNS, trace, tlas,
                         ID3D12Resource_GetGPUVirtualAddress(tlas_scratch) + pass * ((s_tlas.scratch + 255) & ~255ull));
        d3d12_as_barrier(cl, NULL);

        hits[pass] = buffer(D3D12_HEAP_TYPE_DEFAULT, CELLS * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COMMON);
        readback[pass] = buffer(D3D12_HEAP_TYPE_READBACK, CELLS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        d3d12_set_alloc(&rt_set[pass], D3D12_SET_RT, "rt");
        d3d12_set_alloc(&ubo_set[pass], D3D12_SET_UBO, "ubo");
        d3d12_set_write_as(&rt_set[pass], RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, tlas);
        d3d12_set_write_buffer(&ubo_set[pass], GLOBAL_INSTANCE_BUFFER_BINDING_IDX, 0, hits[pass], 0, CELLS * 4);

        ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
        ID3D12GraphicsCommandList4_SetPipelineState(cl, pso);
        d3d12_compute_begin((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER);
        const d3d12_set_t *sets[5] = { &rt_set[pass], &ubo_set[pass], &tex_set, &vb_set, &tile_set };
        d3d12_compute_bind_sets((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, sets);
        uint32_t push[D3D12_PUSH_CONSTANT_DWORDS] = { 0 };
        d3d12_compute_push_constants((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, push, sizeof(push));
        d3d12_compute_dispatch((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, GRID / 8, GRID / 8, 1);

        D3D12_RESOURCE_BARRIER tb = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        tb.Transition.pResource = hits[pass];
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &tb);
        ID3D12GraphicsCommandList4_CopyResource(cl, readback[pass], hits[pass]);

        // executed and finished before the next pass plans, as a frame slot's fence
        ID3D12GraphicsCommandList4_Close(cl);
        ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
        ID3D12CommandQueue_Signal(q, fence, (UINT64)pass + 1);
        ID3D12Fence_SetEventOnCompletion(fence, (UINT64)pass + 1, ev);
        if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("FAIL: pass %d: the GPU did not finish in 10 s\n", pass + 1); return 1; }
        hr = ID3D12Device5_GetDeviceRemovedReason(dev);
        if (FAILED(hr)) { printf("FAIL: pass %d: device removed 0x%08lx\n", pass + 1, (unsigned long)hr); return 1; }
        if (pass < PASSES - 1) {
            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
        }
    }

    int wrong_total = plan_wrong;
    for (int pass = 0; pass < PASSES; pass++) {
        const uint32_t *got = NULL;
        D3D12_RANGE range = { 0, CELLS * 4 };
        ID3D12Resource_Map(readback[pass], 0, &range, (void **)&got);
        int wrong = 0;
        for (int c = 0; c < CELLS; c++)
            if (got[c] != expected(c) && wrong++ < 8)
                printf("  pass %d cell %3d: got %08x, want %08x\n", pass + 1, c, got[c], expected(c));
        printf("pass %d: %d of %d rays wrong\n", pass + 1, wrong, CELLS);
        for (int y = GRID - 1; y >= 0; y--) {
            printf("  ");
            for (int x = 0; x < GRID; x++) {
                const uint32_t g = got[y * GRID + x];
                putchar(g == 0xffffffffu ? '.' : g != expected(y * GRID + x) ? '!' :
                        (g >> 24) == 7 ? 'A' : (g >> 24) == 9 ? 'B' : (g >> 24) == 11 ? 'C' : '?');
            }
            putchar('\n');
        }
        ID3D12Resource_Unmap(readback[pass], 0, NULL);
        wrong_total += wrong;
    }
    // a persistent geometry that changed (one triangle fewer) is built, reuse or not
    {
        d3d12_dynamic_blas_t blases[NUM_RUNS];
        memset(blases, 0, sizeof(blases));
        blases[RUN_B].geometry = d3d12_triangle_run(positions_va, first[RUN_B], run_prims[RUN_B] - 1);
        blases[RUN_B].persistent = blases[RUN_B].reuse = true;
        if (!d3d12_dynamic_blas_plan(dev, &mem[0], blases, NUM_RUNS, trace) || blases[RUN_B].reused ||
            !blases[RUN_B].address) {
            printf("FAIL: a changed persistent geometry was not planned as a build\n");
            wrong_total++;
        } else
            printf("changed persistent geometry: planned as a build, not reused\n");
    }
    printf("\n%s: %d mismatches, %d validation errors\n", wrong_total || errors ? "FAIL" : "PASS", wrong_total, errors);
    for (int pass = 0; pass < PASSES; pass++)
        d3d12_dynamic_as_destroy(&mem[pass]);
    d3d12_as_pool_destroy(&tlas_pool);
    d3d12_bindings_destroy_root_signatures();
    return (wrong_total == 0 && errors == 0) ? 0 : 1;
}
