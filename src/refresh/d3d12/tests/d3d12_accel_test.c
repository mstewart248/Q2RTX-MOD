// Headless check of the acceleration structure layer (accel.c) and of ray
// query through the translated-shader route: a plain-list BLAS and a
// uint16-indexed BLAS (vkpt's two geometry shapes) of the same 2x2 quad, a
// TLAS placing them at x = -2 (z 0, custom index 7) and x = +2 (z 3, index 9),
// and a BLAS of MULTI_GEOMETRIES one-ray quads (custom index 11, z 1) as a
// many-mesh static model has them - past the stack's 32 descs, so every one
// must be there with its own geometry index, as the hit shaders find a mesh by
// it. Then rayquery_test.comp - compiled the way vkpt's shaders are - traces a
// 16x16 grid straight down against the path tracer's root signature. Every
// answer is compared with the analytic one; GPU-based validation is on.
//
// From a vcvars64 prompt, at the repository root:
//   C:/VulkanSDK/1.4.350.0/Bin/glslang.exe -V --target-env vulkan1.2 -S comp src/refresh/d3d12/tests/rayquery_test.comp -o build/tools/rayquery_test.spv
//   build/tools/spv2dxil.exe --dxc C:/VulkanSDK/1.4.350.0/Bin --sm 6_6 build/tools/rayquery_test.spv build/tools/rayquery_test.dxil
//   cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_accel_test.c /Fe:build/tools/ /Fo:build/tools/
//   build/tools/d3d12_accel_test.exe C:/Q2RTX/          (exit 0 = every ray right, validation clean)
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
// the many-geometry BLAS: a quad per ray column (16) in each of 3 bands of y
#define MULTI_COLUMNS       16
#define MULTI_BANDS         3
#define MULTI_GEOMETRIES    (MULTI_COLUMNS * MULTI_BANDS)
#define MULTI_Y0            1.5f    // band b covers y [1.5 + b, 2.5 + b]

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
    memcpy(p, data, (size_t)size);
    ID3D12Resource_Unmap(r, 0, NULL);
    return r;
}

static uint32_t expected(int x, int y)
{
    const float ox = -4.f + (x + .5f) * .5f, oy = -4.f + (y + .5f) * .5f;
    if (oy > -1.f && oy < 1.f) {
        if (ox > -3.f && ox < -1.f) return (7u << 24) | 1000u;    // plane z 0: t = 10, geometry 0
        if (ox > 1.f && ox < 3.f)   return (9u << 24) | 700u;     // plane z 3: t = 7, geometry 0
    }
    if (oy > MULTI_Y0 && oy < MULTI_Y0 + MULTI_BANDS) {           // plane z 1: t = 9
        const uint32_t g = (uint32_t)(oy - MULTI_Y0) * MULTI_COLUMNS + (uint32_t)x;
        return (11u << 24) | (g << 16) | 900u;
    }
    return 0xffffffffu;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "C:/Q2RTX/";
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE get = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE ser =
        (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");

    ID3D12SDKConfiguration1 *cfg = NULL;
    get(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg);
    ID3D12DeviceFactory *f = NULL;
    snprintf(path, sizeof(path), "%sD3D12\\", root);
    if (FAILED(ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, D3D12_SDK_VERSION, path, &IID_ID3D12DeviceFactory, (void **)&f))) {
        printf("no Agility runtime at %s\n", path);
        return 1;
    }
    ID3D12Debug1 *dbg = NULL;
    if (SUCCEEDED(ID3D12DeviceFactory_GetConfigurationInterface(f, &CLSID_D3D12Debug, &IID_ID3D12Debug1, (void **)&dbg))) {
        ID3D12Debug1_EnableDebugLayer(dbg);
        ID3D12Debug1_SetEnableGPUBasedValidation(dbg, TRUE);
    }
    IDXGIFactory6 *xf = NULL; mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    if (FAILED(ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev))) {
        printf("CreateDevice failed (or no ID3D12Device5)\n");
        return 1;
    }
    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device5_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        DWORD cookie; ID3D12InfoQueue1_RegisterMessageCallback(iq, msg, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    }

    // heaps and the binding layer, as device.c sets them up
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
        printf("ROOT SIGNATURES FAILED: %s\n", err);
        return 1;
    }

    // the geometry: one quad, as a plain list and as uint16-indexed
    static const float list_vertices[] = {
        -1, -1, 0,   1, -1, 0,   1, 1, 0,
        -1, -1, 0,   1,  1, 0,  -1, 1, 0,
    };
    static const float indexed_vertices[] = { -1, -1, 0,   1, -1, 0,   1, 1, 0,   -1, 1, 0 };
    static const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
    ID3D12Resource *vb_list = upload(list_vertices, sizeof(list_vertices));
    ID3D12Resource *vb_indexed = upload(indexed_vertices, sizeof(indexed_vertices));
    ID3D12Resource *ib = upload(indices, sizeof(indices));

    const d3d12_as_triangles_t geom_list = { ID3D12Resource_GetGPUVirtualAddress(vb_list), 6, 0, 0 };
    const d3d12_as_triangles_t geom_indexed = { ID3D12Resource_GetGPUVirtualAddress(vb_indexed), 4,
                                                ID3D12Resource_GetGPUVirtualAddress(ib), 6 };
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS trace = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    // the many-geometry BLAS: geometry b * 16 + c is the quad under ray column c in band b
    static float multi_vertices[MULTI_GEOMETRIES * 18];
    for (int b = 0; b < MULTI_BANDS; b++)
        for (int c = 0; c < MULTI_COLUMNS; c++) {
            const float x0 = -4.f + c * .5f, x1 = x0 + .5f, y0 = MULTI_Y0 + b, y1 = y0 + 1.f;
            const float q[18] = { x0, y0, 1, x1, y0, 1, x1, y1, 1, x0, y0, 1, x1, y1, 1, x0, y1, 1 };
            memcpy(multi_vertices + (b * MULTI_COLUMNS + c) * 18, q, sizeof(q));
        }
    ID3D12Resource *vb_multi = upload(multi_vertices, sizeof(multi_vertices));
    d3d12_as_triangles_t geom_multi[MULTI_GEOMETRIES];
    memset(geom_multi, 0, sizeof(geom_multi));
    for (int g = 0; g < MULTI_GEOMETRIES; g++) {
        geom_multi[g].vertices = ID3D12Resource_GetGPUVirtualAddress(vb_multi) + (uint64_t)g * 18 * sizeof(float);
        geom_multi[g].num_vertices = 6;
    }

    d3d12_as_sizes_t s_list, s_indexed, s_multi, s_tlas;
    if (!d3d12_blas_sizes(dev, &geom_list, 1, trace, &s_list) || !d3d12_blas_sizes(dev, &geom_indexed, 1, trace, &s_indexed) ||
        !d3d12_blas_sizes(dev, geom_multi, MULTI_GEOMETRIES, trace, &s_multi) || !d3d12_tlas_sizes(dev, 3, trace, &s_tlas)) {
        printf("FAIL: no prebuild info\n");
        return 1;
    }
    printf("BLAS list %llu B (scratch %llu), BLAS indexed %llu B, TLAS %llu B\n", (unsigned long long)s_list.result,
           (unsigned long long)s_list.scratch, (unsigned long long)s_indexed.result, (unsigned long long)s_tlas.result);

    d3d12_as_pool_t pool;
    if (!d3d12_as_pool_create(&pool, (ID3D12Device *)dev, 1 << 20, L"test AS pool")) {
        printf("FAIL: AS pool\n");
        return 1;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS blas_list = d3d12_as_pool_alloc(&pool, s_list.result);
    const D3D12_GPU_VIRTUAL_ADDRESS blas_indexed = d3d12_as_pool_alloc(&pool, s_indexed.result);
    const D3D12_GPU_VIRTUAL_ADDRESS blas_multi = d3d12_as_pool_alloc(&pool, s_multi.result);
    const D3D12_GPU_VIRTUAL_ADDRESS tlas = d3d12_as_pool_alloc(&pool, s_tlas.result);
    uint64_t scratch_size = s_list.scratch + s_indexed.scratch + s_multi.scratch + s_tlas.scratch + 4 * D3D12_AS_ALIGNMENT;
    ID3D12Resource *scratch = buffer(D3D12_HEAP_TYPE_DEFAULT, scratch_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_COMMON);
    const D3D12_GPU_VIRTUAL_ADDRESS scratch_va = ID3D12Resource_GetGPUVirtualAddress(scratch);
    const uint64_t s1 = (s_list.scratch + D3D12_AS_ALIGNMENT - 1) & ~(uint64_t)(D3D12_AS_ALIGNMENT - 1);
    const uint64_t s2 = s1 + ((s_indexed.scratch + D3D12_AS_ALIGNMENT - 1) & ~(uint64_t)(D3D12_AS_ALIGNMENT - 1));
    const uint64_t s3 = s2 + ((s_multi.scratch + D3D12_AS_ALIGNMENT - 1) & ~(uint64_t)(D3D12_AS_ALIGNMENT - 1));

    // the instances: vkpt's QvkGeometryInstance_t layout
    D3D12_RAYTRACING_INSTANCE_DESC inst[3];
    memset(inst, 0, sizeof(inst));
    inst[0].Transform[0][0] = inst[0].Transform[1][1] = inst[0].Transform[2][2] = 1.f;
    inst[0].Transform[0][3] = -2.f;
    inst[0].InstanceID = 7; inst[0].InstanceMask = 0xff;
    inst[0].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
    inst[0].AccelerationStructure = blas_list;
    inst[1] = inst[0];
    inst[1].Transform[0][3] = 2.f;
    inst[1].Transform[2][3] = 3.f;
    inst[1].InstanceID = 9;
    inst[1].AccelerationStructure = blas_indexed;
    inst[2] = inst[0];
    inst[2].Transform[0][3] = 0.f;
    inst[2].InstanceID = 11;
    inst[2].AccelerationStructure = blas_multi;
    ID3D12Resource *instances = upload(inst, sizeof(inst));

    // the answers and the sets the path tracer layout binds
    ID3D12Resource *hits = buffer(D3D12_HEAP_TYPE_DEFAULT, GRID * GRID * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, GRID * GRID * 4, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    d3d12_set_t rt_set, ubo_set, tex_set, vb_set, tile_set;
    if (!d3d12_set_alloc(&rt_set, D3D12_SET_RT, "rt") || !d3d12_set_alloc(&ubo_set, D3D12_SET_UBO, "ubo") ||
        !d3d12_set_alloc(&tex_set, D3D12_SET_TEXTURES, "textures") ||
        !d3d12_set_alloc(&vb_set, D3D12_SET_VERTEX_BUFFER, "vertex buffer") ||
        !d3d12_set_alloc(&tile_set, D3D12_SET_PT_TILES, "tiles"))
        return 1;
    tex_set.external[0] = 0;
    for (int s = 0; s < D3D12_NUM_GLOBAL_SAMPLERS; s++) {
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(s, &sd);
        d3d12_set_write_sampler(&tex_set, D3D12_BINDING_GLOBAL_SAMPLERS, s, &sd);
    }
    d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, tlas);
    d3d12_set_write_buffer(&ubo_set, GLOBAL_INSTANCE_BUFFER_BINDING_IDX, 0, hits, 0, GRID * GRID * 4);

    snprintf(path, sizeof(path), "%sbuild\\tools\\rayquery_test.dxil", root);
    size_t code_size = 0;
    void *code = load(path, &code_size);
    if (!code) { printf("FAIL: no %s - compile rayquery_test.comp first (see the top of this file)\n", path); return 1; }
    ID3D12PipelineState *pso = d3d12_create_compute_pso(D3D12_PL_PATH_TRACER, code, code_size, L"rayquery test");
    free(code);
    if (!pso) { printf("FAIL: the ray query shader has no PSO against the path tracer layout\n"); return 1; }

    // record: BLASes, barrier, TLAS, barrier, trace, copy back
    ID3D12CommandQueue *q = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12Device5_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca);
    ID3D12GraphicsCommandList4 *cl = NULL;
    ID3D12Device5_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList4, (void **)&cl);

    d3d12_blas_build(cl, &geom_list, 1, trace, blas_list, scratch_va, 0);
    d3d12_blas_build(cl, &geom_indexed, 1, trace, blas_indexed, scratch_va + s1, 0);
    d3d12_blas_build(cl, geom_multi, MULTI_GEOMETRIES, trace, blas_multi, scratch_va + s2, 0);
    d3d12_as_barrier(cl, &pool);
    d3d12_tlas_build(cl, ID3D12Resource_GetGPUVirtualAddress(instances), 3, trace, tlas, scratch_va + s3);
    d3d12_as_barrier(cl, &pool);

    ID3D12DescriptorHeap *heaps[2] = { rheap, sheap };
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
    ID3D12GraphicsCommandList4_SetPipelineState(cl, pso);
    d3d12_compute_begin((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER);
    const d3d12_set_t *sets[5] = { &rt_set, &ubo_set, &tex_set, &vb_set, &tile_set };
    d3d12_compute_bind_sets((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, sets);
    uint32_t push[D3D12_PUSH_CONSTANT_DWORDS] = { 0 };
    d3d12_compute_push_constants((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, push, sizeof(push));
    d3d12_compute_dispatch((ID3D12GraphicsCommandList *)cl, D3D12_PL_PATH_TRACER, GRID / 8, GRID / 8, 1);

    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = hits;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &b);
    ID3D12GraphicsCommandList4_CopyResource(cl, readback, hits);
    ID3D12GraphicsCommandList4_Close(cl);
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);

    ID3D12Fence *fence = NULL;
    ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ID3D12CommandQueue_Signal(q, fence, 1);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("FAIL: the GPU did not finish in 10 s\n"); return 1; }
    HRESULT removed = ID3D12Device5_GetDeviceRemovedReason(dev);
    if (FAILED(removed)) { printf("FAIL: device removed 0x%08lx\n", (unsigned long)removed); return 1; }

    const uint32_t *got = NULL;
    D3D12_RANGE range = { 0, GRID * GRID * 4 };
    ID3D12Resource_Map(readback, 0, &range, (void **)&got);
    int wrong = 0, hit7 = 0, hit9 = 0, hit11 = 0;
    static bool multi_seen[MULTI_GEOMETRIES];
    for (int y = 0; y < GRID; y++) {
        for (int x = 0; x < GRID; x++) {
            const uint32_t g = got[y * GRID + x], e = expected(x, y);
            if (g != e && wrong++ < 10)
                printf("  ray %2d,%2d: got %08x, want %08x\n", x, y, g, e);
            hit7 += g != 0xffffffffu && (g >> 24) == 7;
            hit9 += g != 0xffffffffu && (g >> 24) == 9;
            if (g == e && g != 0xffffffffu && (g >> 24) == 11) {
                hit11++;
                multi_seen[(g >> 16) & 0xff] = true;
            }
        }
    }
    int multi_geometries_hit = 0;
    for (int i = 0; i < MULTI_GEOMETRIES; i++)
        multi_geometries_hit += multi_seen[i];
    for (int y = GRID - 1; y >= 0; y--) {
        printf("  ");
        for (int x = 0; x < GRID; x++) {
            const uint32_t g = got[y * GRID + x];
            putchar(g == 0xffffffffu ? '.' : (g >> 24) == 7 ? 'A' : (g >> 24) == 9 ? 'B' : (g >> 24) == 11 ? 'C' : '?');
        }
        putchar('\n');
    }
    ID3D12Resource_Unmap(readback, 0, NULL);

    printf("\n%d rays wrong of %d (A: list BLAS %d hits, B: indexed BLAS %d hits, C: %d-geometry BLAS %d hits "
           "on %d of its geometries, each by its own index); %d validation errors\n",
           wrong, GRID * GRID, hit7, hit9, MULTI_GEOMETRIES, hit11, multi_geometries_hit, errors);
    d3d12_as_pool_destroy(&pool);
    d3d12_bindings_destroy_root_signatures();
    return (wrong == 0 && multi_geometries_hit == MULTI_GEOMETRIES && errors == 0) ? 0 : 1;
}
