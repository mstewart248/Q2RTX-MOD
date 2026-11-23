// Headless timing of the blood BLAS's padding (Claude, 2026-11-22): blood.c lays
// the section out as fixed-stride droplet slots (BLOOD_SPHERE_FACES of the tess
// level each, 320 at tess 2), a droplet's real faces at the head of its slot and
// the rest padding. The padding has been zeros - degenerate triangles at the
// world origin - and every frame that writes the section builds the BLAS over
// all of it. A triangle whose vertices' x is NaN is INACTIVE (DXR, Vulkan): the
// builder may leave it out. Does it, and do the zeros already cost nothing?
//
// One BLAS of SLOTS droplets three ways, built REPS times each, GPU timestamps:
//   zeros  - the padding as it is
//   nan    - the padding all NaN (every float 0xffffffff, as memset 0xff makes)
//   real   - the real faces alone, packed (the floor)
//
// From a vcvars64 prompt, at the repository root:
//   cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_blood_pad_bench.c /Fe:build/tools/ /Fo:build/tools/
//   build/tools/d3d12_blood_pad_bench.exe C:/Q2RTX/ [slots, default 5120] [stride, default 320]
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPS 40

static ID3D12Device5 *dev;

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

static uint32_t rng = 12345u;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.0f / 16777216.0f); }

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "C:/Q2RTX/";
    const uint32_t slots = argc > 2 ? (uint32_t)atoi(argv[2]) : 5120;
    const uint32_t stride = argc > 3 ? (uint32_t)atoi(argv[3]) : 320;
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE get = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");
    ID3D12SDKConfiguration1 *cfg = NULL;
    get(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg);
    ID3D12DeviceFactory *f = NULL;
    snprintf(path, sizeof(path), "%sD3D12\\", root);
    if (FAILED(ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, D3D12_SDK_VERSION, path, &IID_ID3D12DeviceFactory, (void **)&f))) {
        printf("no Agility runtime at %s\n", path);
        return 1;
    }
    IDXGIFactory6 *xf = NULL; mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    DXGI_ADAPTER_DESC1 desc;
    IDXGIAdapter4_GetDesc1(ad, &desc);
    if (FAILED(ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev))) {
        printf("CreateDevice failed (or no ID3D12Device5)\n");
        return 1;
    }
    printf("%ls: %u slots x %u faces\n", desc.Description, slots, stride);

    // the droplets: 24 fights of a few hundred each, spread over a 4096-unit map;
    // a droplet a small sphere of 64..stride faces (random triangles on it), the
    // way the LOD bands leave settled splats short of the slot
    const uint64_t prims = (uint64_t)slots * stride;
    float *zeros = calloc(prims * 9, sizeof(float));
    float *nans = malloc(prims * 9 * sizeof(float));
    float *packed = malloc(prims * 9 * sizeof(float));
    memset(nans, 0xff, prims * 9 * sizeof(float));
    uint64_t real = 0;
    float fights[24][3];
    for (int i = 0; i < 24; i++) {
        fights[i][0] = frand() * 4096.f - 2048.f;
        fights[i][1] = frand() * 4096.f - 2048.f;
        fights[i][2] = frand() * 512.f - 256.f;
    }
    for (uint32_t s = 0; s < slots; s++) {
        const float *c0 = fights[s % 24];
        const float c[3] = { c0[0] + (frand() - .5f) * 400.f, c0[1] + (frand() - .5f) * 400.f, c0[2] + frand() * 8.f };
        const float r = 1.f + frand() * 5.f;
        const uint32_t faces = 64 + (uint32_t)(frand() * (float)(stride - 64 + 1));
        for (uint32_t t = 0; t < faces && t < stride; t++) {
            float tri[9];
            for (int v = 0; v < 3; v++) {
                const float th = frand() * 6.2831853f, z = frand() * 2.f - 1.f, q = sqrtf(1.f - z * z);
                tri[v * 3 + 0] = c[0] + r * q * cosf(th);
                tri[v * 3 + 1] = c[1] + r * q * sinf(th);
                tri[v * 3 + 2] = c[2] + r * z * .3f;    // splats are flat
            }
            const uint64_t at = ((uint64_t)s * stride + t) * 9;
            memcpy(zeros + at, tri, sizeof(tri));
            memcpy(nans + at, tri, sizeof(tri));
            memcpy(packed + real * 9, tri, sizeof(tri));
            real++;
        }
    }
    printf("real faces %llu of %llu (%.0f%% padding)\n", (unsigned long long)real, (unsigned long long)prims,
           100.0 * (double)(prims - real) / (double)prims);

    struct { const char *name; const float *data; uint64_t prims; ID3D12Resource *vb; } arms[3] = {
        { "zeros", zeros, prims }, { "nan", nans, prims }, { "real", packed, real },
    };
    ID3D12CommandQueue *q = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12Device5_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca);
    ID3D12GraphicsCommandList4 *cl = NULL;
    ID3D12Device5_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList4, (void **)&cl);
    ID3D12Fence *fence = NULL;
    ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    uint64_t fence_value = 0;

    // in video memory, as buf_positions_instanced is (staged through an upload buffer)
    ID3D12Resource *staging[3];
    for (int a = 0; a < 3; a++) {
        const uint64_t size = arms[a].prims * 9 * sizeof(float);
        staging[a] = buffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        arms[a].vb = buffer(D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        if (!staging[a] || !arms[a].vb) { printf("out of memory\n"); return 1; }
        void *p = NULL;
        ID3D12Resource_Map(staging[a], 0, NULL, &p);
        memcpy(p, arms[a].data, (size_t)size);
        ID3D12Resource_Unmap(staging[a], 0, NULL);
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, arms[a].vb, 0, staging[a], 0, size);
        D3D12_RESOURCE_BARRIER tb = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        tb.Transition.pResource = arms[a].vb;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &tb);
    }
    ID3D12GraphicsCommandList4_Close(cl);
    {
        ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
        ID3D12CommandQueue_Signal(q, fence, ++fence_value);
        ID3D12Fence_SetEventOnCompletion(fence, fence_value, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    UINT64 freq = 0;
    ID3D12CommandQueue_GetTimestampFrequency(q, &freq);

    D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * REPS };
    ID3D12QueryHeap *heap = NULL;
    ID3D12Device5_CreateQueryHeap(dev, &qh, &IID_ID3D12QueryHeap, (void **)&heap);
    ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, 2 * REPS * sizeof(UINT64), D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COPY_DEST);

    static const struct { const char *name; D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags; } modes[] = {
        { "fast build", D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD },
        { "fast trace", D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE },
    };
    for (int m = 0; m < 2; m++) {
        for (int a = 0; a < 3; a++) {
            D3D12_RAYTRACING_GEOMETRY_DESC g = { 0 };
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.VertexCount = (UINT)(arms[a].prims * 3);
            g.Triangles.VertexBuffer.StartAddress = ID3D12Resource_GetGPUVirtualAddress(arms[a].vb);
            g.Triangles.VertexBuffer.StrideInBytes = 3 * sizeof(float);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = { 0 };
            bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            bd.Inputs.Flags = modes[m].flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
            bd.Inputs.NumDescs = 1;
            bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            bd.Inputs.pGeometryDescs = &g;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = { 0 };
            ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(dev, &bd.Inputs, &info);
            ID3D12Resource *result = buffer(D3D12_HEAP_TYPE_DEFAULT, info.ResultDataMaxSizeInBytes,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            ID3D12Resource *scratch = buffer(D3D12_HEAP_TYPE_DEFAULT, info.ScratchDataSizeInBytes,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            ID3D12Resource *post = buffer(D3D12_HEAP_TYPE_DEFAULT, 8, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12Resource *post_rb = buffer(D3D12_HEAP_TYPE_READBACK, 8, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!result || !scratch || !post || !post_rb) { printf("out of memory\n"); return 1; }
            bd.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(result);
            bd.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(scratch);

            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            D3D12_RESOURCE_BARRIER uav = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
            for (int r = 0; r < REPS; r++) {
                ID3D12GraphicsCommandList4_EndQuery(cl, heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * r);
                ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cl, &bd, 0, NULL);
                ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &uav);
                ID3D12GraphicsCommandList4_EndQuery(cl, heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * r + 1);
            }
            // the built size: how much of the input the builder kept
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pd = {
                ID3D12Resource_GetGPUVirtualAddress(post), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE };
            D3D12_GPU_VIRTUAL_ADDRESS src = bd.DestAccelerationStructureData;
            ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(cl, &pd, 1, &src);
            D3D12_RESOURCE_BARRIER tb = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            tb.Transition.pResource = post;
            tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &tb);
            ID3D12GraphicsCommandList4_CopyResource(cl, post_rb, post);
            ID3D12GraphicsCommandList4_ResolveQueryData(cl, heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * REPS, readback, 0);
            ID3D12GraphicsCommandList4_Close(cl);
            ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
            ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
            ID3D12CommandQueue_Signal(q, fence, ++fence_value);
            ID3D12Fence_SetEventOnCompletion(fence, fence_value, ev);
            WaitForSingleObject(ev, INFINITE);
            if (FAILED(ID3D12Device5_GetDeviceRemovedReason(dev))) { printf("DEVICE REMOVED\n"); return 1; }

            UINT64 *ts = NULL;
            ID3D12Resource_Map(readback, 0, NULL, (void **)&ts);
            double ms[REPS];
            for (int r = 0; r < REPS; r++)
                ms[r] = (double)(ts[2 * r + 1] - ts[2 * r]) * 1000.0 / (double)freq;
            ID3D12Resource_Unmap(readback, 0, NULL);
            UINT64 *compact = NULL;
            ID3D12Resource_Map(post_rb, 0, NULL, (void **)&compact);
            const UINT64 compacted = compact[0];
            ID3D12Resource_Unmap(post_rb, 0, NULL);
            // the first few warm up
            qsort(ms + 4, REPS - 4, sizeof(double), cmp_double);
            printf("%-10s %-5s %8llu tris: median %.3f ms (min %.3f, max %.3f); compacted %.1f MB of %.1f\n",
                   modes[m].name, arms[a].name, (unsigned long long)arms[a].prims, ms[4 + (REPS - 4) / 2], ms[4],
                   ms[REPS - 1], compacted / 1048576.0, info.ResultDataMaxSizeInBytes / 1048576.0);
            ID3D12Resource_Release(result);
            ID3D12Resource_Release(scratch);
            ID3D12Resource_Release(post);
            ID3D12Resource_Release(post_rb);
        }
    }
    return 0;
}
