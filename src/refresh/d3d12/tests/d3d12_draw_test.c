// Headless render test of the D3D12 2D path: two StretchPic records through
// the translated stretch_pic.vert/.frag, with draw.c's root signature and
// bindings, into a 64x64 B8G8R8A8 target (sRGB RTV), read back.
//   pic 0: untextured red, NDC x -1..0, y -1..0  -> must be the TOP-LEFT quadrant
//   pic 1: 2x2 texture (G B / W R), nearest sampler via the sampler index
//          buffer, NDC x 0..1, y 0..1            -> BOTTOM-RIGHT, texel (0,0) at its top-left
// Then HDR (vid_hdr), as draw.c's second pipeline: stretch_pic.frag.spec_0_1 into an
// R16G16B16A16_FLOAT target with the UBO { ui_hdr_nits 500, saturation 100,
// ui_hdr_video_gamma 2.2 }, a 2x1 texture of sRGB codes 4 and 128 drawn twice:
//   left half: with STRETCH_PIC_FLAG_VIDEO (bit 30) -> (code/255)^2.2 * 500/80
//   right half: without                          -> the piecewise sRGB decode * 500/80
// At code 4 the piecewise toe is ~11x the power curve: the cinematics' block grid.
// Runs with the debug layer and GPU-based validation; any debug-layer error fails it.
// Re-run it after touching spv2dxil, global_textures.h or stretch_pic.*: it is
// what proves the SPIR-V -> HLSL translation draws the right thing.
//
// Build and run from a vcvars64 prompt, after the shaders_d3d12 target:
//   cl /nologo /O2 /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_draw_test.c /Fe:build/tools/
//   build/tools/d3d12_draw_test.exe C:/Q2RTX/        (exit 0 = pass)
// Headless: no window, safe while the game is closed or open.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SPACE(set, binding) (1000u * ((set) + 1u) + (binding))
#define W 64
#define H 64
#define TEX_SLOT 5
#define TEX_SLOT_DARK 6     // the HDR phase's 2x1 texture
#define PIC_FLAG_VIDEO (1u << 30)
#define HEAP_SLOTS 16

typedef struct {
    float x, y, w, h;
    float s, t, w_s, h_t;
    uint32_t color, tex_handle;
    float skew_x, skew_y;
} StretchPic_t;

static ID3D12Device5 *dev;
static int errors;

static void *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*size); fread(p, 1, *size, f); fclose(f);
    return p;
}

static void __stdcall msg(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id, LPCSTR d, void *ctx)
{
    (void)c; (void)ctx;
    if (s <= D3D12_MESSAGE_SEVERITY_WARNING) {
        printf("  [debug %s %d] %s\n", s <= D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "warn", (int)id, d);
        if (s <= D3D12_MESSAGE_SEVERITY_ERROR) errors++;
    }
}

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED 0x%08lx: %s (line %d)\n", hr_, #x, __LINE__); exit(1); } } while (0)

static ID3D12Resource *buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ID3D12Resource *r = NULL;
    CHECK(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r));
    return r;
}

static void *map(ID3D12Resource *r)
{
    void *p = NULL; D3D12_RANGE none = { 0, 0 };
    CHECK(ID3D12Resource_Map(r, 0, &none, &p));
    return p;
}

static float half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, bits;
    if (e == 0) {
        float f = (float)m / 1024.f / 16384.f;      // subnormal: m * 2^-24
        return sign ? -f : f;
    }
    bits = sign | (e == 31 ? 0x7f800000u | (m << 13) : ((e + 112) << 23) | (m << 13));
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = { 0 };
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &br);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "C:\\Q2RTX\\";
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE get = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE ser =
        (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");

    ID3D12SDKConfiguration1 *cfg = NULL;
    CHECK(get(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg));
    ID3D12DeviceFactory *f = NULL;
    snprintf(path, sizeof(path), "%sD3D12\\", root);
    CHECK(ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, D3D12_SDK_VERSION, path, &IID_ID3D12DeviceFactory, (void **)&f));
    ID3D12Debug1 *dbg = NULL;
    if (SUCCEEDED(ID3D12DeviceFactory_GetConfigurationInterface(f, &CLSID_D3D12Debug, &IID_ID3D12Debug1, (void **)&dbg))) {
        ID3D12Debug1_EnableDebugLayer(dbg);
        ID3D12Debug1_SetEnableGPUBasedValidation(dbg, TRUE);
    }
    IDXGIFactory6 *xf = NULL; mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    CHECK(ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev));
    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device5_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        DWORD cookie; ID3D12InfoQueue1_RegisterMessageCallback(iq, msg, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    }
    printf("device up (debug layer + GPU-based validation)\n");

    // ---- root signature: draw.c's
    D3D12_DESCRIPTOR_RANGE1 textures = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, SPACE(1, 0),
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0 };
    D3D12_DESCRIPTOR_RANGE1 samplers = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 4, 0, SPACE(1, 900), D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0 };
    D3D12_ROOT_PARAMETER1 p[5]; memset(p, 0, sizeof(p));
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[0].Descriptor.RegisterSpace = SPACE(0, 0);
    p[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &textures; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[2].Descriptor.RegisterSpace = SPACE(1, 901);
    p[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE; p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[3].Descriptor.RegisterSpace = SPACE(2, 2);
    p[3].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE; p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[4].DescriptorTable.NumDescriptorRanges = 1;
    p[4].DescriptorTable.pDescriptorRanges = &samplers; p[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs = { 0 };
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; rs.Desc_1_1.NumParameters = 5; rs.Desc_1_1.pParameters = p;
    rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ID3DBlob *blob = NULL, *err = NULL;
    CHECK(ser(&rs, &blob, &err));
    ID3D12RootSignature *sig = NULL;
    CHECK(ID3D12Device5_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                            &IID_ID3D12RootSignature, (void **)&sig));

    // ---- PSO
    size_t vs_size, ps_size;
    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\stretch_pic.vert.dxil", root);
    void *vs = load(path, &vs_size);
    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\stretch_pic.frag.dxil", root);
    void *ps = load(path, &ps_size);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
    pd.pRootSignature = sig;
    pd.VS.pShaderBytecode = vs; pd.VS.BytecodeLength = vs_size;
    pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = ps_size;
    D3D12_RENDER_TARGET_BLEND_DESC *bl = &pd.BlendState.RenderTarget[0];
    bl->BlendEnable = TRUE; bl->SrcBlend = D3D12_BLEND_SRC_ALPHA; bl->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    bl->BlendOp = D3D12_BLEND_OP_ADD; bl->SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA; bl->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    bl->BlendOpAlpha = D3D12_BLEND_OP_ADD; bl->LogicOp = D3D12_LOGIC_OP_NOOP; bl->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = NULL;
    CHECK(ID3D12Device5_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

    // ---- queue / list
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *q = NULL; CHECK(ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    ID3D12CommandAllocator *alloc = NULL;
    CHECK(ID3D12Device5_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *cl = NULL;
    CHECK(ID3D12Device5_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));

    // ---- render target (frame target's formats)
    D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { 0 };
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, { 0, 0, 0, 1 } };
    ID3D12Resource *rt = NULL;
    CHECK(ID3D12Device5_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                &IID_ID3D12Resource, (void **)&rt));
    D3D12_DESCRIPTOR_HEAP_DESC rh = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = NULL; CHECK(ID3D12Device5_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv);
    D3D12_RENDER_TARGET_VIEW_DESC rvd = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3D12_RTV_DIMENSION_TEXTURE2D };
    ID3D12Device5_CreateRenderTargetView(dev, rt, &rvd, rtv);

    // ---- 2x2 texture, RGBA8 sRGB: (0,0) green, (1,0) blue, (0,1) white, (1,1) red
    td.Width = 2; td.Height = 2; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; td.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *tex = NULL;
    CHECK(ID3D12Device5_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                &IID_ID3D12Resource, (void **)&tex));
    ID3D12Resource *up = buffer(65536, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t *u = map(up);
    static const uint8_t texels[2][2][4] = { { { 0, 255, 0, 255 }, { 0, 0, 255, 255 } },
                                             { { 255, 255, 255, 255 }, { 255, 0, 0, 255 } } };
    memcpy(u + 0, texels[0], 8);
    memcpy(u + 256, texels[1], 8);              // row pitch 256

    // the queue: offset 1024
    StretchPic_t pics[2] = { 0 };
    pics[0].x = -1; pics[0].y = -1; pics[0].w = 1; pics[0].h = 1;
    pics[0].color = 0xff0000ffu; pics[0].tex_handle = ~0u;       // unpackUnorm4x8: x = low byte = red
    pics[1].x = 0; pics[1].y = 0; pics[1].w = 1; pics[1].h = 1;
    pics[1].s = 0; pics[1].t = 0; pics[1].w_s = 1; pics[1].h_t = 1;
    pics[1].color = 0xffffffffu; pics[1].tex_handle = TEX_SLOT;
    memcpy(u + 1024, pics, sizeof(pics));
    // sampler index table: offset 2048, slot TEX_SLOT -> NEAREST (1)
    uint8_t kinds[64] = { 0 }; kinds[TEX_SLOT] = 1; kinds[TEX_SLOT_DARK] = 1;
    memcpy(u + 2048, kinds, sizeof(kinds));
    // UBO (root CBV straight from the upload heap): offset 4096
    float ubo[4] = { 80, 100, 0, 0 };
    memcpy(u + 4096, ubo, sizeof(ubo));

    ID3D12Resource *sbo = buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *sidx = buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    D3D12_TEXTURE_COPY_LOCATION dst = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    src.PlacedFootprint.Footprint.Width = 2; src.PlacedFootprint.Footprint.Height = 2;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    barrier(cl, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, sbo, 0, up, 1024, sizeof(pics));     // promotes COMMON -> COPY_DEST
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, sidx, 0, up, 2048, sizeof(kinds));
    barrier(cl, sbo, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, sidx, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ---- descriptor heaps: null SRVs everywhere, the texture at TEX_SLOT
    D3D12_DESCRIPTOR_HEAP_DESC sh = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HEAP_SLOTS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *srv_heap = NULL; CHECK(ID3D12Device5_CreateDescriptorHeap(dev, &sh, &IID_ID3D12DescriptorHeap, (void **)&srv_heap));
    UINT inc = ID3D12Device5_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c0; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap, &c0);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    for (int i = 0; i < HEAP_SLOTS; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = { c0.ptr + (SIZE_T)i * inc };
        ID3D12Device5_CreateShaderResourceView(dev, NULL, &sv, h);
    }
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    D3D12_CPU_DESCRIPTOR_HANDLE ht = { c0.ptr + (SIZE_T)TEX_SLOT * inc };
    ID3D12Device5_CreateShaderResourceView(dev, tex, &sv, ht);

    D3D12_DESCRIPTOR_HEAP_DESC smh = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *smp_heap = NULL; CHECK(ID3D12Device5_CreateDescriptorHeap(dev, &smh, &IID_ID3D12DescriptorHeap, (void **)&smp_heap));
    UINT sinc = ID3D12Device5_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_CPU_DESCRIPTOR_HANDLE s0; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(smp_heap, &s0);
    for (int i = 0; i < 4; i++) {
        D3D12_SAMPLER_DESC sd = { 0 };
        // 0 default: linear (would blend the 2x2 texels - must NOT be chosen); 1: nearest
        sd.Filter = i == 1 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sd.MaxLOD = 0; sd.MaxAnisotropy = 1; sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        D3D12_CPU_DESCRIPTOR_HANDLE h = { s0.ptr + (SIZE_T)i * sinc };
        ID3D12Device5_CreateSampler(dev, &sd, h);
    }

    // ---- draw
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, black, 0, NULL);
    ID3D12DescriptorHeap *heaps[2] = { srv_heap, smp_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, sig);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    D3D12_GPU_DESCRIPTOR_HANDLE g0, gs0;
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap, &g0);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(smp_heap, &gs0);
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(sbo));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, g0);
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(sidx));
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 3, ID3D12Resource_GetGPUVirtualAddress(up) + 4096);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 4, gs0);
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 4, 2, 0, 0);

    // ---- read back
    barrier(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *rb = buffer(256 * H, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION rsrc = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; rsrc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION rdst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    rdst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rdst.PlacedFootprint.Footprint.Width = W; rdst.PlacedFootprint.Footprint.Height = H;
    rdst.PlacedFootprint.Footprint.Depth = 1; rdst.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &rdst, 0, 0, 0, &rsrc, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
    ID3D12Fence *fence = NULL; CHECK(ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(ID3D12CommandQueue_Signal(q, fence, 1));
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, 1, ev));
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) { printf("GPU timeout, removed reason 0x%08lx\n", ID3D12Device5_GetDeviceRemovedReason(dev)); return 1; }

    uint8_t *px = NULL; D3D12_RANGE all = { 0, 256 * H };
    CHECK(ID3D12Resource_Map(rb, 0, &all, (void **)&px));

    // expected colours (B,G,R) at sample points; row 0 = top
    struct { int x, y; uint8_t b, g, r; const char *what; } checks[] = {
        {  8,  8,   0,   0, 255, "top-left quadrant: red untextured pic" },
        { 24, 24,   0,   0, 255, "top-left quadrant (inner corner): red" },
        { 56,  8,   0,   0,   0, "top-right: black" },
        {  8, 56,   0,   0,   0, "bottom-left: black" },
        { 40, 40,   0, 255,   0, "bottom-right, its top-left: texel (0,0) green" },
        { 56, 40, 255,   0,   0, "bottom-right, its top-right: texel (1,0) blue" },
        { 40, 56, 255, 255, 255, "bottom-right, its bottom-left: texel (0,1) white" },
        { 56, 56,   0,   0, 255, "bottom-right, its bottom-right: texel (1,1) red" },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        uint8_t *q8 = px + checks[i].y * 256 + checks[i].x * 4;
        bool ok = abs(q8[0] - checks[i].b) <= 2 && abs(q8[1] - checks[i].g) <= 2 && abs(q8[2] - checks[i].r) <= 2;
        printf("%s (%2d,%2d) B%3d G%3d R%3d A%3d  %s\n", ok ? "ok  " : "BAD ", checks[i].x, checks[i].y,
               q8[0], q8[1], q8[2], q8[3], checks[i].what);
        bad += !ok;
    }
    // quad edges exactly on the pixel grid: the red pic covers columns/rows 0..31
    uint8_t *e1 = px + 0 * 256 + 31 * 4, *e2 = px + 0 * 256 + 32 * 4, *e3 = px + 31 * 256 + 0 * 4, *e4 = px + 32 * 256 + 0 * 4;
    bool edges = e1[2] == 255 && e2[2] == 0 && e3[2] == 255 && e4[2] == 0;
    printf("%s red pic edges at x=31|32, y=31|32\n", edges ? "ok  " : "BAD ");
    bad += !edges;

    ID3D12Resource_Unmap(rb, 0, NULL);

    // ---- HDR: draw.c's pipelines[1], the frame target in scRGB
    printf("HDR: stretch_pic.frag.spec_0_1 into R16G16B16A16_FLOAT, ui_hdr_nits 500, ui_hdr_video_gamma 2.2\n");
    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\stretch_pic.frag.spec_0_1.dxil", root);
    void *ps_hdr = load(path, &ps_size);
    pd.PS.pShaderBytecode = ps_hdr; pd.PS.BytecodeLength = ps_size;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D12PipelineState *pso_hdr = NULL;
    CHECK(ID3D12Device5_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_hdr));

    CHECK(ID3D12CommandAllocator_Reset(alloc));
    CHECK(ID3D12GraphicsCommandList_Reset(cl, alloc, NULL));

    td.Width = W; td.Height = H; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv_hdr = { DXGI_FORMAT_R16G16B16A16_FLOAT, { 0, 0, 0, 1 } };
    ID3D12Resource *rt_hdr = NULL;
    CHECK(ID3D12Device5_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv_hdr,
                                                &IID_ID3D12Resource, (void **)&rt_hdr));
    ID3D12DescriptorHeap *rtv_heap_hdr = NULL;
    CHECK(ID3D12Device5_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap_hdr));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_hdr; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap_hdr, &rtv_hdr);
    D3D12_RENDER_TARGET_VIEW_DESC rvd_hdr = { DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2D };
    ID3D12Device5_CreateRenderTargetView(dev, rt_hdr, &rvd_hdr, rtv_hdr);

    // 2x1, sRGB: a near-black code and a mid grey
    static const uint8_t codes[2] = { 4, 128 };
    td.Width = 2; td.Height = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; td.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *dark = NULL;
    CHECK(ID3D12Device5_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                &IID_ID3D12Resource, (void **)&dark));
    for (int i = 0; i < 2; i++) {
        uint8_t texel[4] = { codes[i], codes[i], codes[i], 255 };
        memcpy(u + 16384 + i * 4, texel, 4);
    }
    // the queue: the same texture with the video flag on the left half, without on the right
    StretchPic_t vpics[2] = { 0 };
    for (int i = 0; i < 2; i++) {
        vpics[i].x = i ? 0.f : -1.f; vpics[i].y = -1; vpics[i].w = 1; vpics[i].h = 2;
        vpics[i].s = 0; vpics[i].t = 0; vpics[i].w_s = 1; vpics[i].h_t = 1;
        vpics[i].color = 0xffffffffu;
        vpics[i].tex_handle = TEX_SLOT_DARK | (i ? 0u : PIC_FLAG_VIDEO);
    }
    memcpy(u + 8192, vpics, sizeof(vpics));
    float ubo_hdr[4] = { 500, 100, 2.2f, 0 };
    memcpy(u + 12288, ubo_hdr, sizeof(ubo_hdr));

    ID3D12Resource *sbo_hdr = buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    D3D12_TEXTURE_COPY_LOCATION ddst = { dark, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; ddst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dsrc = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dsrc.PlacedFootprint.Offset = 16384;
    dsrc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    dsrc.PlacedFootprint.Footprint.Width = 2; dsrc.PlacedFootprint.Footprint.Height = 1;
    dsrc.PlacedFootprint.Footprint.Depth = 1; dsrc.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &ddst, 0, 0, 0, &dsrc, NULL);
    barrier(cl, dark, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, sbo_hdr, 0, up, 8192, sizeof(vpics));
    barrier(cl, sbo_hdr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_CPU_DESCRIPTOR_HANDLE hd = { c0.ptr + (SIZE_T)TEX_SLOT_DARK * inc };
    ID3D12Device5_CreateShaderResourceView(dev, dark, &sv, hd);    // sv: R8G8B8A8_UNORM_SRGB, as textures.c

    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv_hdr, black, 0, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, sig);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso_hdr);
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(sbo_hdr));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, g0);
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(sidx));
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 3, ID3D12Resource_GetGPUVirtualAddress(up) + 12288);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 4, gs0);
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv_hdr, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 4, 2, 0, 0);

    barrier(cl, rt_hdr, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *rb_hdr = buffer(512 * H, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION hsrc = { rt_hdr, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; hsrc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION hdst = { rb_hdr, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    hdst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    hdst.PlacedFootprint.Footprint.Width = W; hdst.PlacedFootprint.Footprint.Height = H;
    hdst.PlacedFootprint.Footprint.Depth = 1; hdst.PlacedFootprint.Footprint.RowPitch = 512;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &hdst, 0, 0, 0, &hsrc, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
    CHECK(ID3D12CommandQueue_Signal(q, fence, 2));
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, 2, ev));
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) { printf("GPU timeout, removed reason 0x%08lx\n", ID3D12Device5_GetDeviceRemovedReason(dev)); return 1; }

    uint8_t *hpx = NULL; D3D12_RANGE hall = { 0, 512 * H };
    CHECK(ID3D12Resource_Map(rb_hdr, 0, &hall, (void **)&hpx));
    // columns: video code 4 | video code 128 | plain code 4 | plain code 128
    const float scale = 500.f / 80.f;
    struct { int x; int code; bool video; const char *what; } hchecks[] = {
        {  8, 0, true,  "video, code 4: the 2.2 power curve" },
        { 24, 1, true,  "video, code 128" },
        { 40, 0, false, "plain, code 4: the piecewise sRGB toe" },
        { 56, 1, false, "plain, code 128" },
    };
    float got[4];
    for (int i = 0; i < 4; i++) {
        const uint16_t *h16 = (const uint16_t *)(hpx + 32 * 512 + hchecks[i].x * 8);
        const float c = codes[hchecks[i].code] / 255.f;
        const float want = (hchecks[i].video ? powf(c, 2.2f) : srgb_to_linear(c)) * scale;
        const float r = half_to_float(h16[0]), g = half_to_float(h16[1]), b = half_to_float(h16[2]);
        got[i] = r;
        bool ok = fabsf(r - want) <= 0.03f * want && fabsf(g - r) <= 0.01f * want && fabsf(b - r) <= 0.01f * want;
        printf("%s x=%2d  %.6f %.6f %.6f  want %.6f  %s\n", ok ? "ok  " : "BAD ", hchecks[i].x, r, g, b, want, hchecks[i].what);
        bad += !ok;
    }
    // what the fix is for: near black, the toe is an order of magnitude above the power curve
    bool toe = got[2] > 5.f * got[0];
    printf("%s code 4: plain / video = %.1fx\n", toe ? "ok  " : "BAD ", got[0] > 0.f ? got[2] / got[0] : 0.f);
    bad += !toe;

    printf("%d bad, %d debug-layer errors\n", bad, errors);
    return bad || errors ? 1 : 0;
}
