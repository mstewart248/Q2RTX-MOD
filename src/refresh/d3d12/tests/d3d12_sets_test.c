// Headless check of the binding layer's set instances and dispatch
// (bindings.c) with real vkpt passes: every G-buffer image of LIST_IMAGES made
// at a small render size, the textures set's even and odd instances written
// as vkpt writes them (images, combined samplers, the global samplers, the
// sampler table), a UBO set over zeroed buffers, a vertex buffer set of null
// descriptors - then a handful of post passes dispatched against the STANDARD
// layout under the debug layer with GPU-based validation, a global barrier
// between each. Passes when the device survives and validation said nothing.
//
// It runs real shaders on zeroed inputs, so it picks passes that are a
// per-pixel loop and nothing more (no light lists, no hash probing): a pass
// that loops on data could spin on garbage.
//
// Build and run from a vcvars64 prompt, from the repository root:
//   cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_sets_test.c /Fe:build/tools/ /Fo:build/tools/
//   build/tools/d3d12_sets_test.exe C:/Q2RTX/          (exit 0 = clean)
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bindings.c"

#define RENDER_W    320
#define RENDER_H    180
#define OUTPUT_W    640
#define OUTPUT_H    360

#define HEAP_BINDLESS   MAX_RIMAGES
#define HEAP_SETS       16384
#define SAMPLER_HEAP    2048

static int errors;

static void __stdcall msg(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id, LPCSTR d, void *ctx)
{
    (void)c; (void)ctx;
    if (id == 989)  // CheckFeatureSupport1 advice, for this test's own format queries
        return;
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

static ID3D12Device *dev;

static ID3D12Resource *buffer(uint64_t size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
                                         &IID_ID3D12Resource, (void **)&r);
    return r;
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
    if (FAILED(ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev))) {
        printf("CreateDevice failed\n");
        return 1;
    }
    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        DWORD cookie; ID3D12InfoQueue1_RegisterMessageCallback(iq, msg, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    }

    // the heaps: bindless block, then the set region; samplers: 4 globals, then sets
    ID3D12DescriptorHeap *rheap = NULL, *sheap = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HEAP_BINDLESS + HEAP_SETS,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rheap);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; hd.NumDescriptors = SAMPLER_HEAP;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&sheap);

    d3d12_heaps.device = dev;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rheap, &d3d12_heaps.resource_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(rheap, &d3d12_heaps.resource_gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(sheap, &d3d12_heaps.sampler_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(sheap, &d3d12_heaps.sampler_gpu);
    d3d12_heaps.resource_increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d3d12_heaps.sampler_increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    d3d12_heaps.resource_next = HEAP_BINDLESS;
    d3d12_heaps.resource_end = HEAP_BINDLESS + HEAP_SETS;
    d3d12_heaps.sampler_next = D3D12_NUM_GLOBAL_SAMPLERS;
    d3d12_heaps.sampler_end = SAMPLER_HEAP;

    // the bindless block: null textures, as textures.c leaves unused handles
    for (int i = 0; i < HEAP_BINDLESS; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = d3d12_heaps.resource_cpu;
        h.ptr += (SIZE_T)i * d3d12_heaps.resource_increment;
        ID3D12Device_CreateShaderResourceView(dev, NULL, &sv, h);
    }

    char err[1024] = "";
    if (FAILED(d3d12_bindings_create_root_signatures(dev, ser, err, sizeof(err)))) {
        printf("ROOT SIGNATURES FAILED: %s\n", err);
        return 1;
    }

    // every G-buffer image, as vkpt's are: storage + sampled, no layout changes
    d3d12_extents_t ext = { RENDER_W, RENDER_H, OUTPUT_W, OUTPUT_H, RENDER_W, RENDER_H };
    static ID3D12Resource *img[NUM_VKPT_IMAGES];
    uint64_t total_bytes = 0;
    for (int i = 0; i < NUM_VKPT_IMAGES; i++) {
        uint32_t w, h;
        d3d12_image_extent(i, &ext, &w, &h);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { d3d12_image_format(i) };
        ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs));
        if (!(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) || !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            printf("  note: %s's format %d has no typed UAV load/store\n", d3d12_image_name(i), (int)fs.Format);

        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { 0 };
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = d3d12_image_format(i); rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                        NULL, &IID_ID3D12Resource, (void **)&img[i]))) {
            printf("FAIL: image %s (%ux%u, format %d)\n", d3d12_image_name(i), w, h, (int)rd.Format);
            return 1;
        }
        D3D12_RESOURCE_ALLOCATION_INFO ai;
        ID3D12Device_GetResourceAllocationInfo(dev, &ai, 0, 1, &rd);
        total_bytes += ai.SizeInBytes;
    }
    printf("%d G-buffer images, %.1f MB at %dx%d\n", NUM_VKPT_IMAGES, total_bytes / 1048576.0, RENDER_W, RENDER_H);

    ID3D12Resource *sampler_table = buffer(MAX_RIMAGES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const uint64_t ubo_size = (sizeof(QVKUniformBuffer_t) + 255) & ~255ull;
    ID3D12Resource *ubo = buffer(ubo_size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource *instances = buffer(sizeof(InstanceBuffer), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    printf("UBO %zu bytes, instance buffer %zu bytes\n", sizeof(QVKUniformBuffer_t), sizeof(InstanceBuffer));

    // the sets, as vkpt fills them
    d3d12_set_t textures[2], ubo_set, vb_set;
    for (int odd = 0; odd < 2; odd++) {
        if (!d3d12_set_alloc(&textures[odd], D3D12_SET_TEXTURES, odd ? "textures odd" : "textures even"))
            return 1;
        textures[odd].external[0] = 0;  // the bindless block
        for (int i = 0; i < NUM_VKPT_IMAGES; i++) {
            const int shown = d3d12_image_at_binding(i, odd);
            const unsigned b = d3d12_image_binding(i);
            D3D12_SAMPLER_DESC sd;
            d3d12_global_sampler_desc(d3d12_image_sampler(shown), &sd);
            d3d12_set_write_uav_image(&textures[odd], BINDING_OFFSET_IMAGES + b, 0, img[shown], NULL);
            d3d12_set_write_srv_image(&textures[odd], BINDING_OFFSET_TEXTURES + b, 0, img[shown], NULL, &sd);
        }
        for (int s = 0; s < D3D12_NUM_GLOBAL_SAMPLERS; s++) {
            D3D12_SAMPLER_DESC sd;
            d3d12_global_sampler_desc(s, &sd);
            d3d12_set_write_sampler(&textures[odd], D3D12_BINDING_GLOBAL_SAMPLERS, s, &sd);
        }
        d3d12_set_write_buffer(&textures[odd], D3D12_BINDING_GLOBAL_SAMPLER_INDEX, 0, sampler_table, 0, MAX_RIMAGES);
    }
    if (!d3d12_set_alloc(&ubo_set, D3D12_SET_UBO, "ubo") || !d3d12_set_alloc(&vb_set, D3D12_SET_VERTEX_BUFFER, "vertex buffer"))
        return 1;
    d3d12_set_write_cbv(&ubo_set, GLOBAL_UBO_BINDING_IDX, 0, ID3D12Resource_GetGPUVirtualAddress(ubo), (uint32_t)ubo_size);
    d3d12_set_write_buffer(&ubo_set, GLOBAL_INSTANCE_BUFFER_BINDING_IDX, 0, instances, 0, sizeof(InstanceBuffer));
    printf("sets: %u of %u resource descriptors, %u of %u samplers\n",
           d3d12_heaps.resource_next - HEAP_BINDLESS, HEAP_SETS, d3d12_heaps.sampler_next, SAMPLER_HEAP);

    // the passes
    static const char *passes[] = {
        "checkerboard_interleave.comp", "asvgf_gradient_img.comp", "tone_mapping_histogram.comp", "compositing.comp",
    };
    const int num_passes = (int)(sizeof(passes) / sizeof(passes[0]));
    ID3D12PipelineState *pso[8] = { 0 };
    for (int p = 0; p < num_passes; p++) {
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\%s.dxil", root, passes[p]);
        size_t size = 0;
        void *code = load(path, &size);
        if (!code) { printf("FAIL: no %s\n", path); return 1; }
        pso[p] = d3d12_create_compute_pso(D3D12_PL_STANDARD, code, size, NULL);
        free(code);
        if (!pso[p]) { printf("FAIL: PSO %s\n", passes[p]); return 1; }
    }

    ID3D12CommandQueue *q = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca);
    ID3D12GraphicsCommandList *cl = NULL;
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl);
    ID3D12GraphicsCommandList7 *cl7 = NULL;
    ID3D12GraphicsCommandList_QueryInterface(cl, &IID_ID3D12GraphicsCommandList7, (void **)&cl7);
    printf("barriers: %s\n\n", cl7 ? "enhanced (global)" : "legacy (UAV)");

    ID3D12DescriptorHeap *heaps[2] = { rheap, sheap };
    for (int frame = 0; frame < 2; frame++) {
        for (int p = 0; p < num_passes; p++) {
            ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
            ID3D12GraphicsCommandList_SetPipelineState(cl, pso[p]);
            d3d12_compute_begin(cl, D3D12_PL_STANDARD);
            const d3d12_set_t *sets[3] = { &ubo_set, &textures[frame & 1], &vb_set };
            d3d12_compute_bind_sets(cl, D3D12_PL_STANDARD, sets);
            uint32_t push[D3D12_PUSH_CONSTANT_DWORDS] = { 0 };
            d3d12_compute_push_constants(cl, D3D12_PL_STANDARD, push, sizeof(push));
            d3d12_compute_dispatch(cl, D3D12_PL_STANDARD, (RENDER_W + 15) / 16, (RENDER_H + 15) / 16, 1);

            // vkpt's BARRIER_COMPUTE: every write so far, before any read or write after
            if (cl7) {
                D3D12_GLOBAL_BARRIER gb = { D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE };
                D3D12_BARRIER_GROUP g = { D3D12_BARRIER_TYPE_GLOBAL, 1 };
                g.pGlobalBarriers = &gb;
                ID3D12GraphicsCommandList7_Barrier(cl7, 1, &g);
            } else {
                D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
                ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
            }
            printf("recorded %-32s frame %d (%s set)\n", passes[p], frame, frame & 1 ? "odd" : "even");
        }
    }
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);

    ID3D12Fence *fence = NULL;
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ID3D12CommandQueue_Signal(q, fence, 1);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) {
        printf("FAIL: the GPU did not finish in 10 s\n");
        return 1;
    }
    HRESULT removed = ID3D12Device_GetDeviceRemovedReason(dev);
    printf("\nGPU done; device %s; %d validation errors\n", SUCCEEDED(removed) ? "fine" : "REMOVED", errors);
    if (FAILED(removed))
        printf("removed reason 0x%08lx\n", (unsigned long)removed);
    d3d12_bindings_destroy_root_signatures();
    return (SUCCEEDED(removed) && errors == 0) ? 0 : 1;
}
