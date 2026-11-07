// Headless check of NVIDIA Flow's D3D12 backend (setup/flow/d3d12, built into
// nvflowext.dll by setup/build_flow.py). A device made as d3d12/device.c makes
// it, with the debug layer and GPU-based validation on, hands its ID3D12Device
// and DIRECT queue to Flow through NvFlowD3D12External. Then FRAMES frames of
// what vkpt/flow.c asks of Flow: a burning emitter sphere, a floor as the
// collision mesh (applied after the pressure solve), and the offscreen pass's
// point-light shadow - every one of those passes running as DXIL. Last, the
// density atlas and sparse table the renderer would bind are fetched through
// NvFlowD3D12ExternalGetTexture/GetBuffer and the atlas read back.
//
// From C:/Q2RTX, after setup/build_flow.py:
//   build.ps1 d3d12_flow_test      (cl with /I extern/flow/include/nvflow{,ext})
//   build/tools/d3d12_flow_test.exe C:/Q2RTX/
// Exit 0 = blocks allocated, gas in the atlas, Flow and validation silent.
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

#define FRAMES      60
#define FLOOR_Z     -8.f

static int errors, flow_errors, flow_warnings;
static ID3D12Device *dev;

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
    if (flow_errors + flow_warnings < 40 || level == eNvFlowLogLevel_info)
        printf("  [flow %s] %s\n", level == eNvFlowLogLevel_error ? "error" :
               level == eNvFlowLogLevel_warning ? "warning" : "info", buf);
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

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "C:/Q2RTX/";
    char path[MAX_PATH];

    // --- the device, as d3d12/device.c makes it
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
    ID3D12Debug1 *dbg = NULL;
    if (SUCCEEDED(ID3D12DeviceFactory_GetConfigurationInterface(f, &CLSID_D3D12Debug, &IID_ID3D12Debug1, (void **)&dbg))) {
        ID3D12Debug1_EnableDebugLayer(dbg);
        ID3D12Debug1_SetEnableGPUBasedValidation(dbg, argc > 2 ? FALSE : TRUE);
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

    // --- Flow, as vkpt/flow.c loads it
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
    if (!di) {
        printf("NvFlowGetDeviceInterface(d3d12) gave nothing\n");
        return 1;
    }

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
    grid_desc.maxLocations = 64;
    grid_desc.maxLocationsIsosurface = 16;
    const DWORD t_grid = GetTickCount();
    NvFlowGrid *grid = gi->createGrid(ci, ctx, op_list, ext_op_list, &grid_desc);
    NvFlowGridParamsNamed *named = pi->createGridParamsNamed("d3d12_flow_test");
    NvFlowGridParams *params = named ? pi->mapGridParamsNamed(named) : NULL;
    if (!grid || !params) {
        printf("createGrid failed\n");
        return 1;
    }
    printf("grid created in %lu ms (every pipeline)\n", GetTickCount() - t_grid);

    // --- the scene: one layer, a fireball over a floor, a light beside it
    NvFlowGridSimulateLayerParams layer = NvFlowGridSimulateLayerParams_default;
    layer.luid = 1;
    layer.layer = 0;
    layer.densityCellSize = 1.f;
    layer.physicsCollisionEnabled = NV_FLOW_TRUE;

    NvFlowGridEmitterSphereParams sphere = NvFlowEmitterSphereParams_default;
    sphere.luid = 2;
    sphere.layer = 0;
    sphere.radius = 4.f;
    sphere.velocity.x = sphere.velocity.y = 0.f;
    sphere.velocity.z = 40.f;
    sphere.smoke = 0.5f;

    static const NvFlowFloat3 floor_positions[4] = {
        { -64.f, -64.f, FLOOR_Z }, { 64.f, -64.f, FLOOR_Z }, { 64.f, 64.f, FLOOR_Z }, { -64.f, 64.f, FLOOR_Z } };
    static int floor_indices[6] = { 0, 1, 2, 0, 2, 3 };
    static int floor_counts[2] = { 3, 3 };
    NvFlowGridEmitterMeshParams mesh = NvFlowEmitterMeshParams_default;
    mesh.luid = 0x300;
    mesh.minDistance = -2.f;
    mesh.maxDistance = 1.f;
    mesh.allocateMask = NV_FLOW_FALSE;
    mesh.velocityIsWorldSpace = NV_FLOW_TRUE;
    mesh.velocity.x = mesh.velocity.y = mesh.velocity.z = 0.f;
    mesh.divergence = 0.f;
    mesh.coupleRateVelocity = 1000.f;
    mesh.coupleRateDivergence = 1000.f;
    mesh.coupleRateTemperature = mesh.coupleRateFuel = mesh.coupleRateBurn = mesh.coupleRateSmoke = 0.f;
    mesh.meshPositions = (NvFlowFloat3 *)floor_positions;
    mesh.meshPositionCount = 4;
    mesh.meshPositionVersion = 1;
    mesh.meshFaceVertexIndices = floor_indices;
    mesh.meshFaceVertexIndexCount = 6;
    mesh.meshFaceVertexIndexVersion = 1;
    mesh.meshFaceVertexCounts = floor_counts;
    mesh.meshFaceVertexCountCount = 2;
    mesh.meshFaceVertexCountVersion = 1;
    mesh.applyPostPressure = NV_FLOW_TRUE;
    mesh.isPhysicsCollision = NV_FLOW_TRUE;

    static const float x_points[2] = { 0.f, 1.f };
    static const NvFlowFloat4 rgba_points[2] = { { 1.f, 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f, 1.f } };
    static const float color_scale_points[2] = { 1.f, 1.f };
    NvFlowGridOffscreenLayerParams off = NvFlowGridOffscreenLayerParams_default;
    off.luid = 0x200;
    off.layer = 0;
    off.level = 0;
    off.colormap.resolution = 2;
    off.colormap.xPoints = (float *)x_points;
    off.colormap.xPointCount = 2;
    off.colormap.rgbaPoints = (NvFlowFloat4 *)rgba_points;
    off.colormap.rgbaPointCount = 2;
    off.colormap.colorScalePoints = (float *)color_scale_points;
    off.colormap.colorScalePointCount = 2;
    off.colormap.colorScale = 1.f;
    off.debugVolume.enabled = NV_FLOW_FALSE;
    off.shadow.enabled = NV_FLOW_TRUE;
    off.shadow.isPointLight = NV_FLOW_TRUE;
    off.shadow.lightPosition.x = 24.f;
    off.shadow.lightPosition.y = 0.f;
    off.shadow.lightPosition.z = 8.f;
    off.shadow.attenuation = 0.5f;
    off.shadow.minIntensity = 0.1f;

    NvFlowGridSimulateLayerParams *layer_ptrs[1] = { &layer };
    NvFlowGridEmitterSphereParams *sphere_ptrs[1] = { &sphere };
    NvFlowGridEmitterMeshParams *mesh_ptrs[1] = { &mesh };
    NvFlowGridOffscreenLayerParams *off_ptrs[1] = { &off };
    NvFlowDatabaseTypeSnapshot types[4];

    NvFlowTexture *density = NULL;
    NvFlowBuffer *table = NULL;
    NvFlowUint num_blocks = 0, max_blocks = 0;
    NvFlowUint64 version = 0;
    double sim_time = 0.0;
    const float dt = 1.f / 60.f;
    const DWORD t_frames = GetTickCount();

    for (int frame = 0; frame < FRAMES; frame++) {
        version++;
        types[0] = (NvFlowDatabaseTypeSnapshot) { version, &NvFlowGridSimulateLayerParams_NvFlowReflectDataType, (NvFlowUint8 **)layer_ptrs, 1 };
        types[1] = (NvFlowDatabaseTypeSnapshot) { version, &NvFlowGridEmitterSphereParams_NvFlowReflectDataType, (NvFlowUint8 **)sphere_ptrs, 1 };
        types[2] = (NvFlowDatabaseTypeSnapshot) { version, &NvFlowGridEmitterMeshParams_NvFlowReflectDataType, (NvFlowUint8 **)mesh_ptrs, 1 };
        types[3] = (NvFlowDatabaseTypeSnapshot) { version, &NvFlowGridOffscreenLayerParams_NvFlowReflectDataType, (NvFlowUint8 **)off_ptrs, 1 };
        sim_time += dt;
        NvFlowGridParamsDescSnapshot snap = { { version, types, 4 }, sim_time, dt, NV_FLOW_FALSE };
        pi->commitParams(params, &snap);

        NvFlowGridParamsSnapshot *snapshot = pi->getParamsSnapshot(params, sim_time, 0);
        NvFlowGridParamsDesc pd = { 0 };
        NvFlowTextureAcquire *density_acquire = NULL;
        NvFlowBufferAcquire *table_acquire = NULL;
        if (pi->mapParamsDesc(params, snapshot, &pd)) {
            NvFlowGridRenderData rd = { 0 };
            gi->simulate(ctx, grid, &pd, NV_FLOW_FALSE);
            gi->offscreen(ctx, grid, &pd);
            gi->getRenderData(ctx, grid, &rd);
            if (rd.densityTexture && rd.sparseBuffer && rd.sparseParams.levelCount > 0) {
                num_blocks = rd.sparseParams.levels[0].numLocations;
                if (num_blocks > max_blocks)
                    max_blocks = num_blocks;
                if (num_blocks > 0) {
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
        if (density_acquire) {
            if (density)
                ci->destroyTexture(ctx, density);
            if (table)
                ci->destroyBuffer(ctx, table);
            density = NULL;
            table = NULL;
            ci->getAcquiredTexture(ctx, density_acquire, &density);
            ci->getAcquiredBuffer(ctx, table_acquire, &table);
        }
        if (frame % 15 == 14)
            printf("frame %d: %u blocks\n", frame + 1, num_blocks);
    }
    di->waitIdle(dq);
    printf("%d frames in %lu ms, at most %u blocks\n", FRAMES, GetTickCount() - t_frames, max_blocks);

    int ok = 1;
    if (!density || !table) {
        printf("FAIL: no density atlas or table was acquired\n");
        ok = 0;
    }

    // --- what the renderer would bind
    if (density && table) {
        NvFlowD3D12ExternalTexture tex = { 0 };
        NvFlowD3D12ExternalBuffer buf = { 0 };
        get_texture(density, &tex);
        get_buffer(table, &buf);
        ID3D12Resource *res = (ID3D12Resource *)tex.resource;
        printf("density: %p format %u state 0x%x; table: %p %llu bytes stride %u format %u state 0x%x\n",
               tex.resource, tex.format, tex.state, buf.resource, (unsigned long long)buf.sizeInBytes,
               buf.structureStride, buf.format, buf.state);
        if (!res || tex.format != DXGI_FORMAT_R16G16B16A16_FLOAT || tex.state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
            printf("FAIL: density texture not as documented\n");
            ok = 0;
        }
        if (!buf.resource || buf.sizeInBytes == 0 || buf.state != D3D12_RESOURCE_STATE_COMMON) {
            printf("FAIL: table buffer not as documented\n");
            ok = 0;
        }
        // what d3d12/flow.c's d3d12_flow_bind takes: a 3D texture, and a buffer it can
        // view as a raw UAV (the textures set's FLOW_TABLE binding)
        if (res) {
            D3D12_RESOURCE_DESC dd;
            ID3D12Resource_GetDesc(res, &dd);
            if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                printf("FAIL: the density is not a 3D texture\n");
                ok = 0;
            }
        }
        if (buf.resource) {
            D3D12_RESOURCE_DESC bd;
            ID3D12Resource_GetDesc((ID3D12Resource *)buf.resource, &bd);
            if (bd.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !(bd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
                printf("FAIL: the table cannot be bound as a UAV\n");
                ok = 0;
            }
        }

        if (res) {
            D3D12_RESOURCE_DESC rdesc;
            ID3D12Resource_GetDesc(res, &rdesc);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
            UINT rows; UINT64 row_size, total;
            ID3D12Device_GetCopyableFootprints(dev, &rdesc, 0, 1, 0, &fp, &rows, &row_size, &total);
            printf("atlas %llux%ux%u, %llu bytes\n", (unsigned long long)rdesc.Width, rdesc.Height,
                   (unsigned)rdesc.DepthOrArraySize, (unsigned long long)total);

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
            b.Transition.StateBefore = (D3D12_RESOURCE_STATES)tex.state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
            D3D12_TEXTURE_COPY_LOCATION src = { res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            src.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint = fp;
            ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = (D3D12_RESOURCE_STATES)tex.state;
            ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
            ID3D12GraphicsCommandList_Close(cl);
            ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);

            ID3D12Fence *fence = NULL;
            ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
            ID3D12CommandQueue_Signal(queue, fence, 1);
            HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
            ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
            WaitForSingleObject(ev, INFINITE);

            uint8_t *p = NULL;
            D3D12_RANGE range = { 0, (SIZE_T)total };
            ID3D12Resource_Map(rb, 0, &range, (void **)&p);
            double sum[4] = { 0 };
            float peak[4] = { 0 };
            uint64_t nonzero = 0, bad = 0;
            for (UINT z = 0; z < fp.Footprint.Depth; z++)
                for (UINT y = 0; y < fp.Footprint.Height; y++) {
                    const uint16_t *row = (const uint16_t *)(p + fp.Offset +
                        ((uint64_t)z * fp.Footprint.Height + y) * fp.Footprint.RowPitch);
                    for (UINT x = 0; x < fp.Footprint.Width; x++) {
                        int any = 0;
                        for (int c = 0; c < 4; c++) {
                            const float v = half_to_float(row[x * 4 + c]);
                            if (!isfinite(v)) { bad++; continue; }
                            sum[c] += v;
                            if (v > peak[c]) peak[c] = v;
                            any |= v != 0.f;
                        }
                        nonzero += any;
                    }
                }
            D3D12_RANGE none = { 0, 0 };
            ID3D12Resource_Unmap(rb, 0, &none);
            printf("texels with gas: %llu; non-finite: %llu\n", (unsigned long long)nonzero, (unsigned long long)bad);
            printf("sum   temperature %.1f fuel %.1f burn %.1f smoke %.1f\n", sum[0], sum[1], sum[2], sum[3]);
            printf("peak  temperature %.3f fuel %.3f burn %.3f smoke %.3f\n", peak[0], peak[1], peak[2], peak[3]);
            if (nonzero == 0 || peak[0] <= 0.f) {
                printf("FAIL: the atlas is empty\n");
                ok = 0;
            }
            if (bad) {
                printf("FAIL: non-finite texels\n");
                ok = 0;
            }
            CloseHandle(ev);
            ID3D12Fence_Release(fence);
            ID3D12GraphicsCommandList_Release(cl);
            ID3D12CommandAllocator_Release(ca);
            ID3D12Resource_Release(rb);
        }
    }

    // --- shutdown, as flow_destroy_simulation does it
    di->waitIdle(dq);
    if (density)
        ci->destroyTexture(ctx, density);
    if (table)
        ci->destroyBuffer(ctx, table);
    gi->destroyGrid(ctx, grid);
    pi->destroyGridParamsNamed(named);
    NvFlowUint64 flushed;
    di->flush(dq, &flushed, NULL, NULL);
    di->waitIdle(dq);
    di->destroyDevice(manager, device);
    di->destroyDeviceManager(manager);

    if (max_blocks == 0) {
        printf("FAIL: no blocks were ever allocated\n");
        ok = 0;
    }
    printf("validation errors: %d; Flow errors: %d, warnings: %d\n", errors, flow_errors, flow_warnings);
    if (errors || flow_errors)
        ok = 0;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
