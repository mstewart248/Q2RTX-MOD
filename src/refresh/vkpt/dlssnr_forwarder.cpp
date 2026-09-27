/*
DLSS 5 NEURAL RENDERING (DLSSNR) FORWARDER - built as nvngx.dll_dlssnr.dll

nvngx_dlssnr.dll is the leaked DLSS 5 runtime (NGX feature 18). It is not in the SDK and the
driver's NGX core does not route feature 18 to it, so it is loaded directly and its own
NVSDK_NGX_VULKAN_* exports are called - the same thing OptiScaler's DLSSNR fork does.

The snippet has a caller gate: it resolves the module that owns its caller's return address
and returns FAIL_PlatformError unless that module's PATH contains "nvngx.dll" (the driver core
is _nvngx.dll). q2rtx.exe fails that test, so every call into the snippet is made from this
DLL, whose file name is chosen to pass it. Two consequences worth knowing:

  - NEVER `return snippetFn(...)`. The compiler turns that into a tail jmp, this module's frame
    disappears, and the snippet sees q2rtx.exe as its caller. Every result goes through a
    volatile local first.
  - The engine reaches this DLL through LoadLibrary/GetProcAddress (DLSSNR.c), so a missing
    DLL means "DLSS 5 unavailable", not a failed launch.

The parameter block is our own NVSDK_NGX_Parameter implementation rather than one from the
driver core. The snippet calls it through the vtable, and because this file is compiled by MSVC
from the same SDK header the snippet was, the vtable layout matches by construction - which is
exactly what OptiScaler's "floats are not at slot 1" probing was working around when it drove
the DRIVER's block by hand. Values are stored tagged and converted on Get, so whichever getter
the snippet uses for a key (a resource through Get(void**) or Get(ULL*)) finds it. Keys the
snippet asks for that were never set are remembered, for dlss5_info.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <map>
#include <set>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>

struct ID3D11Resource;
struct ID3D12Resource;
#include <nvsdk_ngx_params.h>

namespace {

struct Value {
    enum Type { ULL, F, D, UI, I, PTR } type;
    union {
        unsigned long long ull;
        float f;
        double d;
        unsigned int ui;
        int i;
        void* ptr;
    };
};

class Params final : public NVSDK_NGX_Parameter {
public:
    void Set(const char* name, unsigned long long v) override { Value x; x.type = Value::ULL; x.ull = v; put(name, x); }
    void Set(const char* name, float v) override { Value x; x.type = Value::F; x.f = v; put(name, x); }
    void Set(const char* name, double v) override { Value x; x.type = Value::D; x.d = v; put(name, x); }
    void Set(const char* name, unsigned int v) override { Value x; x.type = Value::UI; x.ui = v; put(name, x); }
    void Set(const char* name, int v) override { Value x; x.type = Value::I; x.i = v; put(name, x); }
    void Set(const char* name, ID3D11Resource* v) override { Value x; x.type = Value::PTR; x.ptr = v; put(name, x); }
    void Set(const char* name, ID3D12Resource* v) override { Value x; x.type = Value::PTR; x.ptr = v; put(name, x); }
    void Set(const char* name, void* v) override { Value x; x.type = Value::PTR; x.ptr = v; put(name, x); }

    NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override { return num(name, out); }
    NVSDK_NGX_Result Get(const char* name, float* out) const override { return num(name, out); }
    NVSDK_NGX_Result Get(const char* name, double* out) const override { return num(name, out); }
    NVSDK_NGX_Result Get(const char* name, unsigned int* out) const override { return num(name, out); }
    NVSDK_NGX_Result Get(const char* name, int* out) const override { return num(name, out); }
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override { return ptr(name, (void**)out); }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override { return ptr(name, (void**)out); }
    NVSDK_NGX_Result Get(const char* name, void** out) const override { return ptr(name, out); }

    void Reset() override { values.clear(); }

    std::set<std::string> missed;

private:
    std::map<std::string, Value> values;

    void put(const char* name, const Value& v) {
        if (name)
            values[name] = v;
    }

    const Value* find(const char* name) const {
        if (!name)
            return nullptr;
        auto it = values.find(name);
        if (it == values.end()) {
            const_cast<Params*>(this)->missed.insert(name);
            return nullptr;
        }
        return &it->second;
    }

    template <typename T>
    NVSDK_NGX_Result num(const char* name, T* out) const {
        const Value* v = find(name);
        if (!v || !out)
            return NVSDK_NGX_Result_Fail;
        switch (v->type) {
        case Value::ULL: *out = (T)v->ull; break;
        case Value::F:   *out = (T)v->f; break;
        case Value::D:   *out = (T)v->d; break;
        case Value::UI:  *out = (T)v->ui; break;
        case Value::I:   *out = (T)v->i; break;
        case Value::PTR: *out = (T)(unsigned long long)(uintptr_t)v->ptr; break;
        }
        return NVSDK_NGX_Result_Success;
    }

    NVSDK_NGX_Result ptr(const char* name, void** out) const {
        const Value* v = find(name);
        if (!v || !out)
            return NVSDK_NGX_Result_Fail;
        if (v->type == Value::PTR)
            *out = v->ptr;
        else if (v->type == Value::ULL)
            *out = (void*)(uintptr_t)v->ull;
        else
            return NVSDK_NGX_Result_Fail;
        return NVSDK_NGX_Result_Success;
    }
};

typedef NVSDK_NGX_Result(NVSDK_CONV* PFN_Init_Ext)(unsigned long long, const wchar_t*, void*, void*, void*,
                                                    NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
typedef NVSDK_NGX_Result(NVSDK_CONV* PFN_Create)(void*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result(NVSDK_CONV* PFN_Evaluate)(void*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result(NVSDK_CONV* PFN_Release)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result(NVSDK_CONV* PFN_Shutdown1)(void*);

HMODULE      g_module = nullptr;
PFN_Init_Ext g_init = nullptr;
PFN_Create   g_create = nullptr;
PFN_Evaluate g_evaluate = nullptr;
PFN_Release  g_release = nullptr;
PFN_Shutdown1 g_shutdown = nullptr;

const NVSDK_NGX_Feature kFeatureDLSSNR = (NVSDK_NGX_Feature)18;

} // namespace

extern "C" {

// Loads the snippet. Returns a bit per entry point found: init 1, create 2, evaluate 4,
// release 8, shutdown 16. 15 or more means it is usable.
__declspec(dllexport) int dlssnr_load(const wchar_t* snippetPath)
{
    if (!g_module) {
        g_module = LoadLibraryExW(snippetPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_module)
            return 0;
        g_init = (PFN_Init_Ext)GetProcAddress(g_module, "NVSDK_NGX_VULKAN_Init_Ext");
        g_create = (PFN_Create)GetProcAddress(g_module, "NVSDK_NGX_VULKAN_CreateFeature");
        g_evaluate = (PFN_Evaluate)GetProcAddress(g_module, "NVSDK_NGX_VULKAN_EvaluateFeature");
        g_release = (PFN_Release)GetProcAddress(g_module, "NVSDK_NGX_VULKAN_ReleaseFeature");
        g_shutdown = (PFN_Shutdown1)GetProcAddress(g_module, "NVSDK_NGX_VULKAN_Shutdown1");
    }
    return (g_init ? 1 : 0) | (g_create ? 2 : 0) | (g_evaluate ? 4 : 0) | (g_release ? 8 : 0) | (g_shutdown ? 16 : 0);
}

__declspec(dllexport) int dlssnr_init(const wchar_t* dataPath, void* instance, void* physicalDevice, void* device)
{
    if (!g_init)
        return (int)NVSDK_NGX_Result_Fail;
    volatile NVSDK_NGX_Result r = g_init(0, dataPath, instance, physicalDevice, device, NVSDK_NGX_Version_API, nullptr);
    return (int)r;
}

__declspec(dllexport) int dlssnr_shutdown(void* device)
{
    if (!g_shutdown)
        return (int)NVSDK_NGX_Result_Fail;
    volatile NVSDK_NGX_Result r = g_shutdown(device);
    return (int)r;
}

__declspec(dllexport) void* dlssnr_params_create(void) { return static_cast<NVSDK_NGX_Parameter*>(new Params()); }

__declspec(dllexport) void dlssnr_params_destroy(void* p) { delete static_cast<Params*>(static_cast<NVSDK_NGX_Parameter*>(p)); }

__declspec(dllexport) void dlssnr_set_ui(void* p, const char* name, unsigned int v) { static_cast<NVSDK_NGX_Parameter*>(p)->Set(name, v); }
__declspec(dllexport) void dlssnr_set_f(void* p, const char* name, float v) { static_cast<NVSDK_NGX_Parameter*>(p)->Set(name, v); }
__declspec(dllexport) void dlssnr_set_ptr(void* p, const char* name, void* v) { static_cast<NVSDK_NGX_Parameter*>(p)->Set(name, v); }

// Writes the keys the snippet asked for but were never set, one per line.
__declspec(dllexport) int dlssnr_missed_keys(void* p, char* buf, int size)
{
    if (!p || !buf || size <= 0)
        return 0;
    Params* params = static_cast<Params*>(static_cast<NVSDK_NGX_Parameter*>(p));
    int n = 0;
    buf[0] = 0;
    for (const std::string& key : params->missed) {
        int w = _snprintf_s(buf + n, size - n, _TRUNCATE, "%s\n", key.c_str());
        if (w < 0)
            break;
        n += w;
    }
    return (int)params->missed.size();
}

__declspec(dllexport) int dlssnr_create(void* cmdBuffer, void* params, void** outHandle)
{
    if (!g_create || !outHandle)
        return (int)NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Handle* handle = nullptr;
    volatile NVSDK_NGX_Result r = g_create(cmdBuffer, kFeatureDLSSNR, static_cast<NVSDK_NGX_Parameter*>(params), &handle);
    *outHandle = handle;
    return (int)r;
}

__declspec(dllexport) int dlssnr_evaluate(void* cmdBuffer, void* handle, void* params)
{
    if (!g_evaluate)
        return (int)NVSDK_NGX_Result_Fail;
    volatile NVSDK_NGX_Result r = g_evaluate(cmdBuffer, static_cast<NVSDK_NGX_Handle*>(handle),
                                             static_cast<NVSDK_NGX_Parameter*>(params), nullptr);
    return (int)r;
}

__declspec(dllexport) int dlssnr_release(void* handle)
{
    if (!g_release || !handle)
        return (int)NVSDK_NGX_Result_Fail;
    volatile NVSDK_NGX_Result r = g_release(static_cast<NVSDK_NGX_Handle*>(handle));
    return (int)r;
}

} // extern "C"
