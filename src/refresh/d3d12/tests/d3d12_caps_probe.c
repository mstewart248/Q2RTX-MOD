// Headless check of the D3D12 renderer's device path and 2D pipeline:
// Agility runtime via a device factory (retail, then preview), caps, and the
// stretch_pic root signature + PSO from baseq2/shader_d3d12 - the same
// descriptions as src/refresh/d3d12/draw.c. No window.
//
//   cl /nologo /O2 /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_caps_probe.c /Fe:build/tools/
//   build/tools/d3d12_caps_probe.exe C:/Q2RTX/ 0     (1 = the preview runtime; needs Developer Mode)
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define SPACE(set, binding) (1000u * ((set) + 1u) + (binding))

static void *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*size); fread(p, 1, *size, f); fclose(f);
    return p;
}

static void __stdcall msg(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id, LPCSTR d, void *ctx)
{
    (void)c; (void)ctx;
    if (s <= D3D12_MESSAGE_SEVERITY_WARNING)
        printf("  [debug %s %d] %s\n", s <= D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "warn", (int)id, d);
}

static ID3D12DeviceFactory *factory_for(PFN_D3D12_GET_INTERFACE get, UINT ver, const char *path)
{
    ID3D12SDKConfiguration1 *cfg = NULL;
    HRESULT hr = get(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg);
    if (FAILED(hr)) { printf("SDKConfiguration1: 0x%08lx\n", hr); return NULL; }
    ID3D12DeviceFactory *f = NULL;
    hr = ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, ver, path, &IID_ID3D12DeviceFactory, (void **)&f);
    ID3D12SDKConfiguration1_Release(cfg);
    printf("CreateDeviceFactory(%u, %s): 0x%08lx\n", ver, path, hr);
    return SUCCEEDED(hr) ? f : NULL;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "C:\\Q2RTX\\";
    bool preview = argc > 2 && atoi(argv[2]);
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE get = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE ser =
        (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");
    if (!get || !ser || !mkdxgi) { printf("exports missing\n"); return 1; }

    snprintf(path, sizeof(path), "%s%s", root, preview ? "D3D12-preview\\" : "D3D12\\");
    ID3D12DeviceFactory *f = factory_for(get, preview ? D3D12_PREVIEW_SDK_VERSION : D3D12_SDK_VERSION, path);
    if (!f) return 1;
    if (preview) {
        HRESULT ehr = ID3D12DeviceFactory_EnableExperimentalFeatures(f, 1, &D3D12ExperimentalShaderModels, NULL, NULL);
        printf("EnableExperimentalFeatures(shader models): 0x%08lx\n", ehr);
    }

    ID3D12Debug1 *dbg = NULL;
    if (SUCCEEDED(ID3D12DeviceFactory_GetConfigurationInterface(f, &CLSID_D3D12Debug, &IID_ID3D12Debug1, (void **)&dbg))) {
        ID3D12Debug1_EnableDebugLayer(dbg);
        ID3D12Debug1_Release(dbg);
        printf("debug layer on\n");
    }

    IDXGIFactory6 *xf = NULL;
    mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    DXGI_ADAPTER_DESC3 ds; IDXGIAdapter4_GetDesc3(ad, &ds);
    printf("adapter: %ls\n", ds.Description);

    ID3D12Device5 *dev = NULL;
    HRESULT hr = ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev);
    printf("CreateDevice: 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;

    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device5_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        DWORD cookie; ID3D12InfoQueue1_RegisterMessageCallback(iq, msg, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    }

    ID3D12Device16 *d16 = NULL;
    ID3D12Device5_QueryInterface(dev, &IID_ID3D12Device16, (void **)&d16);
    UINT sdkv = preview ? D3D12_PREVIEW_SDK_VERSION : D3D12_SDK_VERSION;
    printf("Device16: %s\n", d16 ? "yes" : "no");
#define CFS(id, p, n) (d16 ? ID3D12Device16_CheckFeatureSupport1(d16, id, p, n, sdkv) : ID3D12Device5_CheckFeatureSupport(dev, id, p, n))
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { 0 };
    for (int m = D3D_HIGHEST_SHADER_MODEL; m >= D3D_SHADER_MODEL_6_0; m--) {
        sm.HighestShaderModel = (D3D_SHADER_MODEL)m;
        if (SUCCEEDED(CFS(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) break;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12));
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16));
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21 = { 0 };
    CFS(D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof(o21));
    D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2 rt2 = { 0 };
    HRESULT rt2hr = CFS(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2, &rt2, sizeof(rt2));
    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT la = { 0 };
    HRESULT lahr = CFS(D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT, &la, sizeof(la));
    printf("shader model %x, raytracing tier %d, enhanced barriers %d, GPU upload heap %d, work graphs %d\n",
           sm.HighestShaderModel, o5.RaytracingTier, o12.EnhancedBarriersSupported, o16.GPUUploadHeapSupported,
           o21.WorkGraphsTier);
    printf("clusters+PTLAS query 0x%08lx -> %d; linear algebra query 0x%08lx -> tier 0x%x\n",
           rt2hr, rt2.ClustersAndPTLASSupported, lahr, la.LinearAlgebraTier);

    // ---- draw.c's root signature, verbatim
    D3D12_DESCRIPTOR_RANGE1 textures = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, SPACE(1, 0),
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0 };
    D3D12_DESCRIPTOR_RANGE1 samplers = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 4, 0, SPACE(1, 900),
        D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0 };
    D3D12_ROOT_PARAMETER1 p[5]; memset(p, 0, sizeof(p));
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[0].Descriptor.RegisterSpace = SPACE(0, 0);
    p[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &textures; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[2].Descriptor.RegisterSpace = SPACE(1, 901);
    p[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[3].Descriptor.RegisterSpace = SPACE(2, 2);
    p[3].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[4].DescriptorTable.NumDescriptorRanges = 1;
    p[4].DescriptorTable.pDescriptorRanges = &samplers; p[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs = { 0 };
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 5; rs.Desc_1_1.pParameters = p;
    rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ID3DBlob *blob = NULL, *err = NULL;
    hr = ser(&rs, &blob, &err);
    printf("serialize root signature: 0x%08lx %s\n", hr, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "");
    if (FAILED(hr)) return 1;
    ID3D12RootSignature *sig = NULL;
    hr = ID3D12Device5_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                           &IID_ID3D12RootSignature, (void **)&sig);
    printf("CreateRootSignature: 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;

    size_t vs_size, ps_size;
    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\stretch_pic.vert.dxil", root);
    void *vs = load(path, &vs_size);
    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\stretch_pic.frag.dxil", root);
    void *ps = load(path, &ps_size);
    if (!vs || !ps) return 1;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
    pd.pRootSignature = sig;
    pd.VS.pShaderBytecode = vs; pd.VS.BytecodeLength = vs_size;
    pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = ps_size;
    D3D12_RENDER_TARGET_BLEND_DESC *bl = &pd.BlendState.RenderTarget[0];
    bl->BlendEnable = TRUE; bl->SrcBlend = D3D12_BLEND_SRC_ALPHA; bl->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    bl->BlendOp = D3D12_BLEND_OP_ADD; bl->SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
    bl->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bl->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bl->LogicOp = D3D12_LOGIC_OP_NOOP; bl->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = NULL;
    hr = ID3D12Device5_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    printf("CreateGraphicsPipelineState(stretch_pic): 0x%08lx\n", hr);

    // every translated compute shader must at least be accepted by the driver
    // with SOME root signature; check they load as DXIL the runtime takes, by
    // asking for their reflection-free pipeline with an empty-but-matching
    // signature is not possible generically - so just count signed DXIL here
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\*.dxil", root);
    HANDLE h = FindFirstFileA(path, &fd);
    int n = 0, unsigned_n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char full[MAX_PATH]; size_t sz;
            snprintf(full, sizeof(full), "%sbaseq2\\shader_d3d12\\%s", root, fd.cFileName);
            unsigned char *b = load(full, &sz);
            if (b && sz > 20) {
                int zero = 1;
                for (int i = 4; i < 20; i++) if (b[i]) zero = 0;
                if (zero) { unsigned_n++; printf("  unsigned: %s\n", fd.cFileName); }
            }
            free(b); n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    printf("%d DXIL files, %d unsigned\n", n, unsigned_n);

    if (pso) ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(sig);
    ID3D12Device5_Release(dev);
    return FAILED(hr) ? 1 : 0;
}
