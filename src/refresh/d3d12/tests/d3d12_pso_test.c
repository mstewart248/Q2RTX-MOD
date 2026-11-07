// Headless check of the binding layer (bindings.c) against every translated
// compute shader: build the root signature of each of vkpt's pipeline layouts
// on the real device, then make each baseq2/shader_d3d12/*.comp.dxil and
// *.query.dxil into a compute PSO against each until one is accepted. D3D12
// refuses a PSO whose shader names a register its root signature does not
// cover with the right type and size, so a shader no layout takes is a binding
// the tables in bindings.c miss - the debug layer's message says which.
// The SER A/B's ray generation libraries (*.raygen.dxil, *.ser.dxil) are made into
// state objects with the path tracer layout's root signature, as world.c does.
// Then passes.c's pass table, each pass against the layout it names, as world.c makes
// them - one that fails there fails the whole 3D view.
//
// Build and run from a vcvars64 prompt, from the repository root:
//   cl /nologo /O2 /I extern/agility-sdk/include src/refresh/d3d12/tests/d3d12_pso_test.c /Fe:build/tools/
//   build/tools/d3d12_pso_test.exe C:/Q2RTX/          (exit 0 = every shader has a layout)
// Headless: no window.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bindings.c"
#include "../passes.c"

static char messages[16384];
static size_t messages_len;

static void __stdcall msg(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id, LPCSTR d, void *ctx)
{
    (void)c; (void)ctx; (void)id;
    if (s <= D3D12_MESSAGE_SEVERITY_ERROR && messages_len < sizeof(messages) - 2) {
        int n = snprintf(messages + messages_len, sizeof(messages) - messages_len, "      %s\n", d);
        if (n > 0)
            messages_len += (size_t)n;
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

static bool ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !_stricmp(s + a - b, tail);
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
    if (SUCCEEDED(ID3D12DeviceFactory_GetConfigurationInterface(f, &CLSID_D3D12Debug, &IID_ID3D12Debug1, (void **)&dbg)))
        ID3D12Debug1_EnableDebugLayer(dbg);
    IDXGIFactory6 *xf = NULL; mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    ID3D12Device *dev = NULL;
    if (FAILED(ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev))) {
        printf("CreateDevice failed\n");
        return 1;
    }
    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        DWORD cookie; ID3D12InfoQueue1_RegisterMessageCallback(iq, msg, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    }

    d3d12_bindings_build_layouts();
    for (int i = 0; i < D3D12_NUM_SET_LAYOUTS; i++)
        printf("set %-14s %3d bindings, %4u resource + %3u sampler descriptors\n", d3d12_set_layouts[i].name,
               d3d12_set_layouts[i].num_bindings, d3d12_set_layouts[i].num_resources, d3d12_set_layouts[i].num_samplers);
    printf("%d G-buffer images\n", d3d12_num_images());

    char err[1024] = "";
    if (FAILED(d3d12_bindings_create_root_signatures(dev, ser, err, sizeof(err)))) {
        printf("ROOT SIGNATURES FAILED: %s\n%s", err, messages);
        return 1;
    }
    printf("%d root signatures made\n\n", D3D12_NUM_PIPELINE_LAYOUTS);

    int total = 0, matched = 0;
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\*.dxil", root);
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no DXIL in %s\n", path); return 1; }
    do {
        // and their specialization variants, foo.comp.spec_0_1.dxil
        if (!ends_with(fd.cFileName, ".comp.dxil") && !ends_with(fd.cFileName, ".query.dxil") &&
            !strstr(fd.cFileName, ".comp.spec_") && !strstr(fd.cFileName, ".query.spec_"))
            continue;
        // ray generation libraries: below
        if (ends_with(fd.cFileName, ".raygen.dxil") || ends_with(fd.cFileName, ".ser.dxil"))
            continue;
        total++;
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\%s", root, fd.cFileName);
        size_t size = 0;
        void *code = load(path, &size);

        int ok = -1;
        char best[sizeof(messages)] = "";
        for (int l = 0; l < D3D12_NUM_PIPELINE_LAYOUTS && ok < 0; l++) {
            messages_len = 0; messages[0] = 0;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = { 0 };
            pd.pRootSignature = d3d12_pipeline_layouts[l].root_signature;
            pd.CS.pShaderBytecode = code;
            pd.CS.BytecodeLength = size;
            ID3D12PipelineState *pso = NULL;
            if (SUCCEEDED(ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso))) {
                ok = l;
                ID3D12PipelineState_Release(pso);
            } else {
                // every layout's refusal: the first mismatch each one found
                size_t room = sizeof(best) - strlen(best) - 1;
                char line[160];
                snprintf(line, sizeof(line), "    [%s]", d3d12_pipeline_layouts[l].name);
                strncat(best, line, room);
                room = sizeof(best) - strlen(best) - 1;
                strncat(best, messages_len ? messages + 5 : " (no message)\n", room);
            }
        }
        if (ok >= 0) {
            matched++;
            printf("ok    %-40s %s\n", fd.cFileName, d3d12_pipeline_layouts[ok].name);
        } else {
            printf("FAIL  %-40s no layout takes it:\n%s", fd.cFileName, best);
        }
        free(code);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    printf("\n%d of %d compute shaders have a layout\n", matched, total);

    // the pass table, each against its own layout (world.c's compute_pso)
    int passes_ok = 0;
    for (int i = 0; i < D3D12_NUM_PASSES; i++) {
        const d3d12_pass_info_t *pi = &d3d12_pass_info[i];
        if (!pi->shader) {
            printf("FAIL  pass %d has no shader\n", i);
            continue;
        }
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\%s.dxil", root, pi->shader);
        size_t size = 0;
        void *code = load(path, &size);
        if (!code) {
            printf("FAIL  pass %d: no %s.dxil\n", i, pi->shader);
            continue;
        }
        messages_len = 0; messages[0] = 0;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = { 0 };
        pd.pRootSignature = d3d12_pipeline_layouts[pi->layout].root_signature;
        pd.CS.pShaderBytecode = code;
        pd.CS.BytecodeLength = size;
        ID3D12PipelineState *pso = NULL;
        if (SUCCEEDED(ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso))) {
            passes_ok++;
            ID3D12PipelineState_Release(pso);
        } else {
            printf("FAIL  pass %d %s against %s:\n%s", i, pi->shader, d3d12_pipeline_layouts[pi->layout].name,
                   messages_len ? messages : "      (no message)\n");
        }
        free(code);
    }
    printf("%d of %d passes made against their own layouts\n", passes_ok, (int)D3D12_NUM_PASSES);

    // the SER A/B's (world.c's ser_ready): needs shader model 6.9
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_9 };
    if (FAILED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
    d3d12_heaps.device = dev;
    int rg_total = 0, rg_ok = 0;
    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\*.dxil", root);
    h = FindFirstFileA(path, &fd);
    do {
        if (!ends_with(fd.cFileName, ".raygen.dxil") && !ends_with(fd.cFileName, ".ser.dxil"))
            continue;
        rg_total++;
        if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_9) {
            printf("FAIL  %-48s shader model %d.%d, not 6.9\n", fd.cFileName, sm.HighestShaderModel >> 4,
                   sm.HighestShaderModel & 15);
            continue;
        }
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\%s", root, fd.cFileName);
        size_t size = 0;
        void *code = load(path, &size);
        messages_len = 0; messages[0] = 0;
        uint8_t id[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = { 0 };
        ID3D12StateObject *so = code ? d3d12_create_raygen_object(D3D12_PL_PATH_TRACER, code, size, id, NULL) : NULL;
        if (so) {
            rg_ok++;
            printf("ok    %-48s state object, %s\n", fd.cFileName, d3d12_pipeline_layouts[D3D12_PL_PATH_TRACER].name);
            ID3D12StateObject_Release(so);
        } else {
            printf("FAIL  %-48s no state object:\n%s", fd.cFileName, messages_len ? messages : "      (no message)\n");
        }
        free(code);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("%d of %d ray generation libraries made into state objects\n\n", rg_ok, rg_total);

    // the graphics pipelines the renderer makes, each as world.c / draw.c make it
    static const struct { const char *vs, *ps; int layout; } gfx[] = {
        { "final_blit.vert", "film_grain.frag", D3D12_PL_UBO_TEXTURES },     // world.c's debug blit
        { "final_blit.vert", "crt.frag", D3D12_PL_UBO_TEXTURES },            // ...with pt_crt
    };
    int gfx_ok = 0;
    for (int i = 0; i < (int)(sizeof(gfx) / sizeof(gfx[0])); i++) {
        size_t vs_size = 0, ps_size = 0;
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\%s.dxil", root, gfx[i].vs);
        void *vs = load(path, &vs_size);
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\%s.dxil", root, gfx[i].ps);
        void *ps = load(path, &ps_size);
        if (!vs || !ps) {
            printf("FAIL  %s + %s: no DXIL\n", gfx[i].vs, gfx[i].ps);
            continue;
        }
        messages_len = 0; messages[0] = 0;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
        pd.pRootSignature = d3d12_pipeline_layouts[gfx[i].layout].root_signature;
        pd.VS.pShaderBytecode = vs;
        pd.VS.BytecodeLength = vs_size;
        pd.PS.pShaderBytecode = ps;
        pd.PS.BytecodeLength = ps_size;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;    // D3D12_FRAME_TARGET_RTV_FORMAT
        pd.SampleDesc.Count = 1;
        ID3D12PipelineState *pso = NULL;
        if (SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso))) {
            gfx_ok++;
            printf("ok    %s + %s  %s\n", gfx[i].vs, gfx[i].ps, d3d12_pipeline_layouts[gfx[i].layout].name);
            ID3D12PipelineState_Release(pso);
        } else {
            printf("FAIL  %s + %s against %s:\n%s", gfx[i].vs, gfx[i].ps, d3d12_pipeline_layouts[gfx[i].layout].name,
                   messages_len ? messages : "      (no message)\n");
        }
        free(vs);
        free(ps);
    }
    const int gfx_total = (int)(sizeof(gfx) / sizeof(gfx[0]));
    printf("%d of %d graphics pipelines made\n", gfx_ok, gfx_total);

    d3d12_bindings_destroy_root_signatures();
    return matched == total && gfx_ok == gfx_total && rg_ok == rg_total && passes_ok == D3D12_NUM_PASSES ? 0 : 1;
}
