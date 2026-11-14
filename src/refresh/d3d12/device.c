/*
D3D12 device: the Agility SDK runtime, adapter, device, queue, fence, heaps,
deferred release and uploads. See d3d12pt.h and ROADMAP.md.
*/

#define D3D12PT_DEFINE_GUIDS
#include "d3d12pt.h"
#include <SDL.h>    // d3d12_parallel_for's threads
#include <time.h>   // the device-removed report's date

d3d12_state_t d3d;

static cvar_t *d3d12_debug;             // 0 off, 1 debug layer, 2 + GPU-based validation
static cvar_t *d3d12_agility_preview;   // 1 = the preview runtime (clusters, PTLAS); needs Developer Mode
static cvar_t *d3d12_adapter;           // -1 = the first hardware adapter, by GPU preference

/* ------------------------------------------------------------------------- */
/* d3d12.dll AND dxgi.dll ARE LOADED HERE, NOT LINKED                         */
/* ------------------------------------------------------------------------- */

/* A static import would load both into every q2rtx.exe - the Vulkan and GL
   renderers too - and D3D12GetInterface, which the Agility SDK needs, only
   exists in d3d12.dll on Windows 10 1909 and later: on an older one the exe
   would not start at all. */
static HMODULE d3d12_dll, dxgi_dll;
static PFN_D3D12_CREATE_DEVICE          pD3D12CreateDevice;
static PFN_D3D12_GET_DEBUG_INTERFACE    pD3D12GetDebugInterface;
static PFN_D3D12_GET_INTERFACE          pD3D12GetInterface;
static PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE pD3D12SerializeVersionedRootSignature;
typedef HRESULT (WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT flags, REFIID riid, void **factory);
static PFN_CREATE_DXGI_FACTORY2         pCreateDXGIFactory2;

/* THE AGILITY SDK FOR THE WHOLE PROCESS (11-06). NGX will not start on a device made
   through a device factory (create_device_factory, d3d12_device_init), but it does on
   the same 1.620 runtime chosen by the D3D12SDKVersion export, and SER needs that
   runtime (shader model 6.9). So the export is written here, at run time, just before
   this renderer first loads d3d12.dll - the loader reads it then - and only when
   D3D12\D3D12Core.dll is beside the exe: a version with no runtime behind it makes every
   D3D12 device in the process fail (0x887e0003). Probed 11-06 (written 0: the system
   runtime, SM 6.8; written 620: D3D12\D3D12Core.dll, SM 6.9; 620 without the folder:
   0x887e0003).
   A session that never starts vid_rtx 3 keeps the system runtime, the Vulkan
   renderer's D3D12 users included; one that does moves them all to 1.620, a newer
   runtime of the same API. If something loaded d3d12.dll first (an overlay, a
   d3d12_agility_preview start) the export is left alone and the device factory is
   used, as before. d3d12.dll stays loaded, so the choice holds for the process. */
__declspec(dllexport) UINT D3D12SDKVersion = 0;
__declspec(dllexport) const char *D3D12SDKPath = ".\\D3D12\\";

static bool process_agility;    // the export was written before d3d12.dll loaded

// <the exe's folder>\<sub>
static bool exe_dir_path(const char *sub, char *path, size_t size)
{
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)size);
    char *slash = (n && n < size) ? strrchr(path, '\\') : NULL;
    if (!slash)
        return false;
    slash[1] = 0;
    Q_strlcat(path, sub, size);
    return true;
}

static void export_agility_runtime(void)
{
    char core[MAX_OSPATH];
    if (process_agility || GetModuleHandleA("d3d12.dll") || !exe_dir_path("D3D12\\D3D12Core.dll", core, sizeof(core)) ||
        GetFileAttributesA(core) == INVALID_FILE_ATTRIBUTES)
        return;
    D3D12SDKVersion = D3D12_SDK_VERSION;
    process_agility = true;
}

/* THE PREVIEW RUNTIME'S MENU ROW (d3d12_agility_preview_available, q2rtx.menu): only
   where it can start - built with the preview SDK's headers, its D3D12Core.dll in
   D3D12-preview\ beside the exe, and Windows Developer Mode on, without which the
   runtime refuses to load (D3D12_ERROR_INVALID_REDIST). Developer Mode is the
   AppModelUnlock value that Settings > System > For developers writes. Matt (11-06):
   "only show the setting if the developer mode is on". */
static bool windows_developer_mode(void)
{
    DWORD value = 0, size = sizeof(value);
    return RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
                        "AllowDevelopmentWithoutDevLicense", RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

static bool preview_runtime_available(void)
{
#ifdef D3D12_PREVIEW_SDK_VERSION
    char core[MAX_OSPATH];
    return windows_developer_mode() && exe_dir_path("D3D12-preview\\D3D12Core.dll", core, sizeof(core)) &&
           GetFileAttributesA(core) != INVALID_FILE_ATTRIBUTES;
#else
    return false;
#endif
}

// after the first device: did D3D12Core.dll come from D3D12\ beside the exe?
static bool process_runtime_is_agility(void)
{
    char want[MAX_OSPATH], got[MAX_OSPATH];
    HMODULE core = GetModuleHandleA("D3D12Core.dll");
    if (!core || !exe_dir_path("D3D12\\", want, sizeof(want)) || !GetModuleFileNameA(core, got, sizeof(got)))
        return false;
    return !Q_strncasecmp(got, want, strlen(want));
}

// `agility`: the retail Agility runtime for the process (export_agility_runtime)
static bool load_dlls(bool agility)
{
    if (agility)
        export_agility_runtime();
    if (!d3d12_dll)
        d3d12_dll = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi_dll)
        dxgi_dll = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d3d12_dll || !dxgi_dll) {
        Com_EPrintf("D3D12: d3d12.dll or dxgi.dll is missing\n");
        return false;
    }
    pD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(d3d12_dll, "D3D12CreateDevice");
    pD3D12GetDebugInterface = (PFN_D3D12_GET_DEBUG_INTERFACE)(void *)GetProcAddress(d3d12_dll, "D3D12GetDebugInterface");
    pD3D12GetInterface = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12_dll, "D3D12GetInterface");
    pD3D12SerializeVersionedRootSignature = (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(void *)
        GetProcAddress(d3d12_dll, "D3D12SerializeVersionedRootSignature");
    pCreateDXGIFactory2 = (PFN_CREATE_DXGI_FACTORY2)(void *)GetProcAddress(dxgi_dll, "CreateDXGIFactory2");
    if (!pD3D12CreateDevice || !pCreateDXGIFactory2 || !pD3D12SerializeVersionedRootSignature) {
        Com_EPrintf("D3D12: this Windows's d3d12.dll / dxgi.dll are too old\n");
        return false;
    }
    return true;
}

HRESULT d3d12_serialize_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob,
                                       ID3DBlob **error)
{
    *blob = NULL;
    if (error)
        *error = NULL;
    if (!pD3D12SerializeVersionedRootSignature)
        return E_NOTIMPL;
    return pD3D12SerializeVersionedRootSignature(desc, blob, error);
}

/* ------------------------------------------------------------------------- */

bool d3d12_check(HRESULT hr, const char *what, const char *file, int line)
{
    if (SUCCEEDED(hr))
        return true;

    Com_EPrintf("D3D12: %s failed: 0x%08lx (%s:%d)\n", what, (unsigned long)hr, file, line);
    if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) && d3d.device) {
        Com_EPrintf("D3D12: device removed, reason 0x%08lx\n",
                    (unsigned long)ID3D12Device5_GetDeviceRemovedReason(d3d.device));
        // the report waits for the main thread (d3d12_device_lost): this may be a worker
        d3d.device_lost = true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* WHEN THE GPU STOPS (d3d12_dred)                                            */
/* ------------------------------------------------------------------------- */

/* DRED - Device Removed Extended Data - is set before the device is made and read
   after it is removed.
   - d3d12_dred 1 (the default): the runtime keeps every resource's GPU addresses, so a
     page fault names the resource it fell in, or the one freed under it. Nothing a frame.
   - 2: auto-breadcrumbs as well - the GPU writes how far it got through each command
     list - with the profiler's pass names as markers in the frame's list
     (d3d12_timer_mark), so a hang names its pass. That costs some GPU time: it is for
     chasing a hang.
   CVAR_REFRESH: a change restarts the renderer.

   Why (11-13): the landing pod exploding at the start of mgu1m1 hung the GPU
   (DXGI_ERROR_DEVICE_HUNG at Present; nvlddmkm event 153 in the System log) and the
   next frame's NVIDIA Flow step died on the removed device - an access violation in
   nvflowext.dll (its createBuffer came back NULL), which looked like Flow's own bug and
   said nothing about where the GPU had stopped. Now the first frame after a removal
   writes this report to the console and to logs/gpu_crash.txt, which a relaunch
   does not overwrite, and ends the game with a plain error (R_BeginFrame_D3D12); Flow
   is not stepped on a removed device (frontend.c). */
static cvar_t *d3d12_dred;

static void set_dred(int level)
{
    static bool was_set;    // the runtime's DRED settings outlive a device: turn them off again
    if (level <= 0 && !was_set)
        return;

    ID3D12DeviceRemovedExtendedDataSettings1 *s = NULL;
    HRESULT hr = E_NOINTERFACE;
    if (d3d.device_factory)
        hr = ID3D12DeviceFactory_GetConfigurationInterface(d3d.device_factory, &CLSID_D3D12DeviceRemovedExtendedData,
                                                           &IID_ID3D12DeviceRemovedExtendedDataSettings1, (void **)&s);
    else if (pD3D12GetDebugInterface)
        hr = pD3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings1, (void **)&s);
    if (FAILED(hr) || !s) {
        Com_WPrintf("D3D12: no DRED settings (0x%08lx) - d3d12_dred does nothing\n", (unsigned long)hr);
        return;
    }
    const D3D12_DRED_ENABLEMENT on = D3D12_DRED_ENABLEMENT_FORCED_ON, off = D3D12_DRED_ENABLEMENT_FORCED_OFF;
    ID3D12DeviceRemovedExtendedDataSettings1_SetPageFaultEnablement(s, level >= 1 ? on : off);
    ID3D12DeviceRemovedExtendedDataSettings1_SetAutoBreadcrumbsEnablement(s, level >= 2 ? on : off);
    ID3D12DeviceRemovedExtendedDataSettings1_SetBreadcrumbContextEnablement(s, level >= 2 ? on : off);
    ID3D12DeviceRemovedExtendedDataSettings1_Release(s);
    was_set = level > 0;
    d3d.dred = max(level, 0);
    if (level >= 2)
        Com_Printf("D3D12: DRED breadcrumbs on (d3d12_dred 2): a GPU hang names its pass, at some GPU cost\n");
}

static const char *const dred_ops[] = {
    "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect",
    "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
    "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
    "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame",
    "ProcessFrames", "AtomicCopyBufferUINT", "AtomicCopyBufferUINT64", "ResolveSubresourceRegion",
    "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
    "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
    "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand",
    "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
    "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata", "Barrier",
    "BeginCommandList",
};

static const char *dred_op_name(D3D12_AUTO_BREADCRUMB_OP op, char *buf, size_t size)
{
    if ((unsigned)op < ARRAYSIZE(dred_ops))
        return dred_ops[op];
    Q_snprintf(buf, size, "op %d", (int)op);
    return buf;
}

static const char *dred_name(const char *a, const wchar_t *w, char *buf, size_t size, const char *none)
{
    if (a && *a)
        return a;
    if (w && *w && WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)size, NULL, NULL) > 0)
        return buf;
    return none;
}

static void dred_line(qhandle_t f, const char *fmt, ...) q_printf(2, 3);
static void dred_line(qhandle_t f, const char *fmt, ...)
{
    char buf[MAXPRINTMSG];
    va_list ap;
    va_start(ap, fmt);
    Q_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Com_EPrintf("%s\n", buf);
    if (f)
        FS_FPrintf(f, "%s\n", buf);
}

static void dred_breadcrumbs(qhandle_t f, ID3D12DeviceRemovedExtendedData1 *dred)
{
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 out = { 0 };
    HRESULT hr = ID3D12DeviceRemovedExtendedData1_GetAutoBreadcrumbsOutput1(dred, &out);
    if (FAILED(hr)) {
        dred_line(f, "breadcrumbs: none (0x%08lx)%s", (unsigned long)hr,
                  d3d.dred < 2 ? " - d3d12_dred 2 records which pass the GPU was in" : "");
        return;
    }

    int finished = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE1 *n = out.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
        char lname[256], qname[256], opbuf[32];
        // ours are all named: a list without one is NVIDIA Flow's (or a DLSS library's)
        const char *list = dred_name(n->pCommandListDebugNameA, n->pCommandListDebugNameW, lname, sizeof(lname),
                                     "(no name: NVIDIA Flow's, or DLSS's)");
        const char *queue = dred_name(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW, qname, sizeof(qname),
                                      "(no name)");
        const UINT count = n->BreadcrumbCount;
        const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        if (count && done >= count) {
            finished++;
            continue;
        }
        if (!done) {
            dred_line(f, "list \"%s\" on \"%s\": not started (%u operations)", list, queue, count);
            continue;
        }

        // the one it was in: the first operation not done, and the pass marker before it
        dred_line(f, "list \"%s\" on \"%s\": STOPPED in operation %u of %u, %s", list, queue, done + 1, count,
                  dred_op_name(n->pCommandHistory[done], opbuf, sizeof(opbuf)));
        const wchar_t *pass = NULL;
        UINT pass_index = 0;
        for (UINT c = 0; c < n->BreadcrumbContextsCount; c++) {
            const D3D12_DRED_BREADCRUMB_CONTEXT *ctx = &n->pBreadcrumbContexts[c];
            if (ctx->BreadcrumbIndex <= done && ctx->pContextString && (!pass || ctx->BreadcrumbIndex >= pass_index)) {
                pass = ctx->pContextString;
                pass_index = ctx->BreadcrumbIndex;
            }
        }
        if (pass) {
            char pbuf[256];
            dred_line(f, "  after marker \"%s\" (operation %u)", dred_name(NULL, pass, pbuf, sizeof(pbuf), "?"),
                      pass_index + 1);
        }
        const UINT first = done > 6 ? done - 6 : 0, last = min(count, done + 4);
        for (UINT i = first; i < last; i++)
            dred_line(f, "  %s %u %s", i == done ? "->" : i < done ? "  " : " .", i + 1,
                      dred_op_name(n->pCommandHistory[i], opbuf, sizeof(opbuf)));
    }
    if (finished)
        dred_line(f, "%d more command lists had finished", finished);
}

static const char *dred_allocation_type(D3D12_DRED_ALLOCATION_TYPE type, char *buf, size_t size)
{
    switch (type) {
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:           return "resource";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP:               return "heap";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:    return "descriptor heap";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP:         return "query heap";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:       return "command list";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR:  return "command allocator";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:     return "pipeline";
    case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT:       return "state object";
    default:
        Q_snprintf(buf, size, "type %d", (int)type);
        return buf;
    }
}

static void dred_allocations(qhandle_t f, const char *what, const D3D12_DRED_ALLOCATION_NODE1 *n)
{
    int shown = 0, total = 0;
    for (; n; n = n->pNext, total++) {
        if (shown == 24)
            continue;
        char nbuf[256], tbuf[32];
        dred_line(f, "  %s: %s (%s)", what, dred_name(n->ObjectNameA, n->ObjectNameW, nbuf, sizeof(nbuf), "(no name)"),
                  dred_allocation_type(n->AllocationType, tbuf, sizeof(tbuf)));
        shown++;
    }
    if (total > shown)
        dred_line(f, "  ... and %d more %s", total - shown, what);
    if (!total)
        dred_line(f, "  %s: nothing", what);
}

static void report_device_removed(void)
{
    const HRESULT reason = ID3D12Device5_GetDeviceRemovedReason(d3d.device);
    qhandle_t f = 0;
    if (FS_OpenFile("logs/gpu_crash.txt", &f, FS_MODE_APPEND | FS_FLAG_TEXT) < 0)
        f = 0;

    char date[64] = "";
    const time_t now = time(NULL);
    const struct tm *tm = localtime(&now);
    if (tm)
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", tm);
    const char *why = reason == DXGI_ERROR_DEVICE_HUNG ? "DXGI_ERROR_DEVICE_HUNG: the GPU hung or faulted" :
                      reason == DXGI_ERROR_DEVICE_REMOVED ? "DXGI_ERROR_DEVICE_REMOVED" :
                      reason == DXGI_ERROR_DEVICE_RESET ? "DXGI_ERROR_DEVICE_RESET" :
                      reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "DXGI_ERROR_DRIVER_INTERNAL_ERROR" :
                      reason == E_OUTOFMEMORY ? "E_OUTOFMEMORY" : "?";
    dred_line(f, "==== %s D3D12 device removed: 0x%08lx (%s), map %s, frame %llu, d3d12_dred %d ====", date,
              (unsigned long)reason, why, Cvar_VariableString("mapname"), (unsigned long long)d3d.frame_counter,
              d3d.dred);

    ID3D12DeviceRemovedExtendedData1 *dred = NULL;
    if (FAILED(ID3D12Device5_QueryInterface(d3d.device, &IID_ID3D12DeviceRemovedExtendedData1, (void **)&dred)) ||
        !dred) {
        dred_line(f, "no DRED report%s", d3d.dred ? "" : " (d3d12_dred 0)");
    } else {
        ID3D12DeviceRemovedExtendedData2 *dred2 = NULL;
        if (SUCCEEDED(ID3D12DeviceRemovedExtendedData1_QueryInterface(dred, &IID_ID3D12DeviceRemovedExtendedData2,
                                                                      (void **)&dred2)) && dred2) {
            const D3D12_DRED_DEVICE_STATE state = ID3D12DeviceRemovedExtendedData2_GetDeviceState(dred2);
            dred_line(f, "device state: %s", state == D3D12_DRED_DEVICE_STATE_HUNG ? "HUNG (a timeout: still running)" :
                      state == D3D12_DRED_DEVICE_STATE_FAULT ? "FAULT" :
                      state == D3D12_DRED_DEVICE_STATE_PAGEFAULT ? "PAGEFAULT (a bad memory access)" : "unknown");
            ID3D12DeviceRemovedExtendedData2_Release(dred2);
        }

        dred_breadcrumbs(f, dred);

        D3D12_DRED_PAGE_FAULT_OUTPUT1 pf = { 0 };
        const HRESULT hr = ID3D12DeviceRemovedExtendedData1_GetPageFaultAllocationOutput1(dred, &pf);
        if (FAILED(hr)) {
            dred_line(f, "page fault: none recorded (0x%08lx)", (unsigned long)hr);
        } else if (!pf.PageFaultVA) {
            dred_line(f, "page fault: none");
        } else {
            dred_line(f, "page fault at GPU address 0x%016llx", (unsigned long long)pf.PageFaultVA);
            dred_allocations(f, "in", pf.pHeadExistingAllocationNode);
            dred_allocations(f, "freed", pf.pHeadRecentFreedAllocationNode);
        }
        ID3D12DeviceRemovedExtendedData1_Release(dred);
    }

    if (f) {
        FS_FPrintf(f, "\n");
        FS_CloseFile(f);
        Com_EPrintf("D3D12: this report is also in logs/gpu_crash.txt\n");
    }
}

bool d3d12_device_lost(void)
{
    if (!d3d.device)
        return false;
    if (!d3d.device_lost) {
        if (ID3D12Device5_GetDeviceRemovedReason(d3d.device) == S_OK)
            return false;
        d3d.device_lost = true;
    }
    if (!d3d.device_reported) {
        d3d.device_reported = true;
        report_device_removed();
    }
    return true;
}

void d3d12_set_name(ID3D12Object *object, const char *name)
{
    WCHAR wide[256];
    if (!object || !name)
        return;
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, (int)ARRAYSIZE(wide)) > 0)
        ID3D12Object_SetName(object, wide);
}

/* ------------------------------------------------------------------------- */
/* FENCE                                                                      */
/* ------------------------------------------------------------------------- */

uint64_t d3d12_signal(void)
{
    uint64_t value = ++d3d.fence_last_signalled;
    D3D_CHECK(ID3D12CommandQueue_Signal(d3d.queue, d3d.fence, value));
    return value;
}

bool d3d12_fence_done(uint64_t value)
{
    return ID3D12Fence_GetCompletedValue(d3d.fence) >= value;
}

void d3d12_wait_fence(uint64_t value)
{
    if (!value || !d3d.fence || d3d12_fence_done(value))
        return;

    if (!D3D_CHECK(ID3D12Fence_SetEventOnCompletion(d3d.fence, value, d3d.fence_event)))
        return;

    // never forever: a removed device never signals, and an endless wait here
    // is a hung game with nothing in the log
    while (WaitForSingleObject(d3d.fence_event, 2000) == WAIT_TIMEOUT) {
        HRESULT reason = ID3D12Device5_GetDeviceRemovedReason(d3d.device);
        if (FAILED(reason)) {
            d3d12_check(reason, "waiting for the GPU", __FILE__, __LINE__);
            return;
        }
        Com_WPrintf("D3D12: still waiting for the GPU (fence %llu, at %llu)\n",
                    (unsigned long long)value, (unsigned long long)ID3D12Fence_GetCompletedValue(d3d.fence));
    }
}

void d3d12_wait_idle(void)
{
    if (d3d.queue && d3d.fence)
        d3d12_wait_fence(d3d12_signal());
}

/* ------------------------------------------------------------------------- */
/* DEFERRED RELEASE                                                           */
/* ------------------------------------------------------------------------- */

/* An object released while a frame is being recorded may be used by that very
   frame, whose fence value is not known until R_EndFrame signals it - and an
   upload flush can signal a smaller value in between. So such entries wait
   with fence 0 ("the open frame") and R_EndFrame stamps them through
   d3d12_stamp_garbage. Between frames, everything already submitted is the
   most that can still be using an object. */
typedef struct {
    IUnknown    *object;
    uint64_t    fence;
} garbage_t;

static garbage_t *garbage;
static int num_garbage, max_garbage;

void d3d12_defer_release_at(IUnknown *object, uint64_t value)
{
    if (!object)
        return;
    if (num_garbage == max_garbage) {
        max_garbage = max_garbage ? max_garbage * 2 : 256;
        garbage = Z_Realloc(garbage, max_garbage * sizeof(garbage[0]));
    }
    garbage[num_garbage].object = object;
    garbage[num_garbage].fence = value;
    num_garbage++;
}

void d3d12_defer_release(IUnknown *object)
{
    d3d12_defer_release_at(object, d3d.frame_open ? 0 : d3d.fence_last_signalled);
}

void d3d12_stamp_garbage(uint64_t frame_fence)
{
    for (int i = 0; i < num_garbage; i++)
        if (!garbage[i].fence)
            garbage[i].fence = frame_fence;
}

void d3d12_collect_garbage(bool all)
{
    uint64_t done = (all || !d3d.fence) ? UINT64_MAX : ID3D12Fence_GetCompletedValue(d3d.fence);
    int kept = 0;
    for (int i = 0; i < num_garbage; i++) {
        if (all || (garbage[i].fence && garbage[i].fence <= done))
            IUnknown_Release(garbage[i].object);
        else
            garbage[kept++] = garbage[i];
    }
    num_garbage = kept;
    if (all) {
        Z_Free(garbage);
        garbage = NULL;
        max_garbage = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* DESCRIPTORS AND BUFFERS                                                    */
/* ------------------------------------------------------------------------- */

D3D12_CPU_DESCRIPTOR_HANDLE d3d12_srv_cpu(UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d3d.srv_heap, &h);
    h.ptr += (SIZE_T)index * d3d.srv_increment;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE d3d12_srv_gpu(UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d3d.srv_heap, &h);
    h.ptr += (UINT64)index * d3d.srv_increment;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE d3d12_sampler_gpu(UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d3d.sampler_heap, &h);
    h.ptr += (UINT64)index * d3d.sampler_increment;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE d3d12_rtv(UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d3d.rtv_heap, &h);
    h.ptr += (SIZE_T)index * d3d.rtv_increment;
    return h;
}

static ID3D12Resource *create_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_HEAP_FLAGS heap_flags, const char *name);

ID3D12Resource *d3d12_create_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                    const char *name)
{
    return create_buffer(size, heap, flags, D3D12_HEAP_FLAG_NONE, name);
}

/* `heap_flags` D3D12_HEAP_FLAG_CREATE_NOT_ZEROED: for memory written whole before it is
   read (the upload chunks) - a 512 MB batch of a map load's textures need not be
   zeroed first. A runtime that refuses the flag gets the buffer made without it. */
static ID3D12Resource *create_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_HEAP_FLAGS heap_flags, const char *name)
{
    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type = heap;

    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size ? size : 1;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;

    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (heap == D3D12_HEAP_TYPE_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;

    ID3D12Resource *res = NULL;
    if (heap_flags != D3D12_HEAP_FLAG_NONE &&
        SUCCEEDED(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, heap_flags, &rd, state, NULL,
                                                        &IID_ID3D12Resource, (void **)&res))) {
        d3d12_set_name((ID3D12Object *)res, name);
        return res;
    }
    res = NULL;
    if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                         &IID_ID3D12Resource, (void **)&res)))
        return NULL;
    d3d12_set_name((ID3D12Object *)res, name);
    return res;
}

/* ------------------------------------------------------------------------- */
/* UPLOADS                                                                    */
/* ------------------------------------------------------------------------- */

/* Staging memory comes from a pool of persistently mapped upload chunks, and the
   copies are recorded into a pool of command lists: both are reused once the
   fence says the GPU is past the batch that used them, so a frame that uploads
   a cinematic frame or a few new textures allocates nothing. A chunk bigger
   than the standard size (one huge texture) is released instead of pooled. */
#define UPLOAD_CHUNK_SIZE   (32u << 20)
#define MAX_UPLOAD_CHUNKS   32
#define MAX_UPLOAD_LISTS    8

typedef struct {
    ID3D12Resource  *buffer;
    uint8_t         *map;
    uint64_t        size;
    uint64_t        fence;      // free once the fence is here
    bool            in_batch;   // written by the batch being recorded
} upload_chunk_t;

typedef struct {
    ID3D12CommandAllocator      *allocator;
    ID3D12GraphicsCommandList   *cmd;
    uint64_t                    fence;
} upload_list_t;

static struct {
    upload_list_t   lists[MAX_UPLOAD_LISTS];
    int             num_lists;
    upload_list_t   *open;      // the batch being recorded, or NULL
    upload_chunk_t  chunks[MAX_UPLOAD_CHUNKS];
    int             num_chunks;
    int             current;    // chunk being filled, -1 = none
    uint64_t        used;       // bytes of it
} up = { .current = -1 };

static bool upload_open(void)
{
    if (up.open)
        return true;

    upload_list_t *list = NULL;
    for (int i = 0; i < up.num_lists && !list; i++)
        if (d3d12_fence_done(up.lists[i].fence))
            list = &up.lists[i];

    if (!list && up.num_lists < MAX_UPLOAD_LISTS) {
        list = &up.lists[up.num_lists];
        if (!D3D_CHECK(ID3D12Device5_CreateCommandAllocator(d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            &IID_ID3D12CommandAllocator, (void **)&list->allocator)))
            return false;
        // created closed, like the frame list: reset below
        if (!D3D_CHECK(ID3D12Device5_CreateCommandList1(d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        D3D12_COMMAND_LIST_FLAG_NONE, &IID_ID3D12GraphicsCommandList,
                                                        (void **)&list->cmd))) {
            D3D12_RELEASE(list->allocator);
            return false;
        }
        d3d12_set_name((ID3D12Object *)list->cmd, "uploads");
        list->fence = 0;
        up.num_lists++;
    }

    if (!list) {
        // every list is still on the GPU: wait for the oldest
        list = &up.lists[0];
        for (int i = 1; i < up.num_lists; i++)
            if (up.lists[i].fence < list->fence)
                list = &up.lists[i];
        d3d12_wait_fence(list->fence);
    }

    if (!D3D_CHECK(ID3D12CommandAllocator_Reset(list->allocator)) ||
        !D3D_CHECK(ID3D12GraphicsCommandList_Reset(list->cmd, list->allocator, NULL)))
        return false;

    up.open = list;
    return true;
}

static int upload_take_chunk(uint64_t min_size)
{
    for (int i = 0; i < up.num_chunks; i++) {
        upload_chunk_t *c = &up.chunks[i];
        if (!c->in_batch && c->size >= min_size && d3d12_fence_done(c->fence))
            return i;
    }

    if (up.num_chunks == MAX_UPLOAD_CHUNKS) {
        // the pool is full of chunks the GPU still reads: send this batch,
        // wait for everything, and every chunk is free again
        d3d12_upload_flush();
        d3d12_wait_idle();
        if (!upload_open())
            return -1;
        for (int i = 0; i < up.num_chunks; i++)
            if (up.chunks[i].size >= min_size)
                return i;
        // none big enough: replace the last one
        upload_chunk_t *last = &up.chunks[--up.num_chunks];
        D3D12_RELEASE(last->buffer);
    }

    upload_chunk_t *c = &up.chunks[up.num_chunks];
    memset(c, 0, sizeof(*c));
    c->size = max(UPLOAD_CHUNK_SIZE, min_size);
    c->buffer = create_buffer(c->size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, "upload chunk");
    if (!c->buffer)
        return -1;
    D3D12_RANGE none = { 0, 0 };
    void *map = NULL;
    if (!D3D_CHECK(ID3D12Resource_Map(c->buffer, 0, &none, &map))) {
        D3D12_RELEASE(c->buffer);
        return -1;
    }
    c->map = map;
    return up.num_chunks++;
}

static uint8_t *upload_alloc(uint64_t size, uint64_t align, ID3D12Resource **buffer, uint64_t *offset)
{
    if (!upload_open())
        return NULL;

    uint64_t at = up.current >= 0 ? (up.used + align - 1) & ~(align - 1) : 0;
    if (up.current < 0 || at + size > up.chunks[up.current].size) {
        int i = upload_take_chunk(size);
        if (i < 0)
            return NULL;
        up.current = i;
        up.chunks[i].in_batch = true;
        at = 0;
    }

    upload_chunk_t *c = &up.chunks[up.current];
    up.used = at + size;
    *buffer = c->buffer;
    *offset = at;
    return c->map + at;
}

bool d3d12_upload_subresource(ID3D12Resource *dst, uint32_t subresource, const void *pixels, int width, int height,
                              int bytes_per_pixel)
{
    D3D12_RESOURCE_DESC desc;
    ID3D12Resource_GetDesc(dst, &desc);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size, total;
    ID3D12Device5_GetCopyableFootprints(d3d.device, &desc, subresource, 1, 0, &fp, &rows, &row_size, &total);

    ID3D12Resource *src;
    uint64_t offset;
    uint8_t *out = upload_alloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &src, &offset);
    if (!out)
        return false;

    // a 3D image's subresource is every slice, `rows` rows of RowPitch each;
    // the pixels come slice after slice, tightly packed
    const uint8_t *in = pixels;
    size_t in_pitch = (size_t)width * bytes_per_pixel;
    size_t copy = min(in_pitch, (size_t)row_size);
    for (UINT z = 0; z < fp.Footprint.Depth; z++)
        for (UINT y = 0; y < rows && y < (UINT)height; y++)
            memcpy(out + ((size_t)z * rows + y) * fp.Footprint.RowPitch, in + ((size_t)z * height + y) * in_pitch, copy);

    D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
    to.pResource = dst;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = subresource;
    from.pResource = src;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = fp;
    from.PlacedFootprint.Offset = offset;
    ID3D12GraphicsCommandList_CopyTextureRegion(up.open->cmd, &to, 0, 0, 0, &from, NULL);
    return true;
}

void d3d12_upload_texture(ID3D12Resource *dst, const void *pixels, int width, int height, int bytes_per_pixel)
{
    if (!d3d12_upload_subresource(dst, 0, pixels, width, height, bytes_per_pixel))
        return;

    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = dst;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(up.open->cmd, 1, &b);
}

void d3d12_upload_buffer(ID3D12Resource *dst, uint64_t offset, const void *data, uint64_t size)
{
    ID3D12Resource *src;
    uint64_t src_offset;
    uint8_t *out = upload_alloc(size, 16, &src, &src_offset);
    if (!out)
        return;
    memcpy(out, data, size);
    ID3D12GraphicsCommandList_CopyBufferRegion(up.open->cmd, dst, offset, src, src_offset, size);
}

uint8_t *d3d12_upload_reserve(uint64_t size, ID3D12Resource **buffer, uint64_t *offset)
{
    // one allocation: a chunk of its own when bigger than the pooled size (released
    // once the batch is done, as any giant)
    return upload_alloc(size ? size : 1, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, buffer, offset);
}

ID3D12GraphicsCommandList *d3d12_upload_list(void)
{
    return upload_open() ? up.open->cmd : NULL;
}

/* ------------------------------------------------------------------------- */
/* PARALLEL FOR                                                               */
/* ------------------------------------------------------------------------- */

/* Loading faster (Matt, 11-12): the map load's textures, the G-buffer and the
   start-up pipelines are each many independent ID3D12Device calls, and the device
   is free-threaded. Threads are made for the call and gone after it - a few
   hundred microseconds for all of them, against hundreds of milliseconds of work. */
#define PARALLEL_MAX_THREADS    64

typedef struct {
    d3d12_job_fn    fn;
    void            *ctx;
    int             count;
    SDL_atomic_t    next;
} parallel_t;

static void parallel_run(parallel_t *p)
{
    int i;
    while ((i = SDL_AtomicAdd(&p->next, 1)) < p->count)
        p->fn(p->ctx, i);
}

static int SDLCALL parallel_worker(void *arg)
{
    parallel_run(arg);
    return 0;
}

int d3d12_parallel_for(int count, d3d12_job_fn fn, void *ctx, int max_threads)
{
    if (count <= 0)
        return 0;
    int threads = max_threads > 0 ? max_threads : SDL_GetCPUCount();
    threads = threads < 1 ? 1 : threads > PARALLEL_MAX_THREADS ? PARALLEL_MAX_THREADS : threads;
    if (threads > count)
        threads = count;

    parallel_t p;
    p.fn = fn;
    p.ctx = ctx;
    p.count = count;
    SDL_AtomicSet(&p.next, 0);

    SDL_Thread *workers[PARALLEL_MAX_THREADS];
    int started = 0;
    for (int t = 1; t < threads; t++) {
        workers[started] = SDL_CreateThread(parallel_worker, "d3d12 jobs", &p);
        if (workers[started])
            started++;
    }
    parallel_run(&p);   // and whatever no worker took, if none started
    for (int t = 0; t < started; t++)
        SDL_WaitThread(workers[t], NULL);
    return started + 1;
}

void d3d12_upload_flush(void)
{
    if (!up.open)
        return;

    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)up.open->cmd };
    if (D3D_CHECK(ID3D12GraphicsCommandList_Close(up.open->cmd)))
        ID3D12CommandQueue_ExecuteCommandLists(d3d.queue, 1, lists);
    uint64_t value = d3d12_signal();

    up.open->fence = value;
    up.open = NULL;

    int kept = 0;
    for (int i = 0; i < up.num_chunks; i++) {
        upload_chunk_t c = up.chunks[i];
        if (c.in_batch) {
            c.in_batch = false;
            c.fence = value;
            if (c.size > UPLOAD_CHUNK_SIZE) {
                // a one-off giant: gone once this batch is done
                ID3D12Resource_Unmap(c.buffer, 0, NULL);
                d3d12_defer_release_at((IUnknown *)c.buffer, value);
                continue;
            }
        }
        up.chunks[kept++] = c;
    }
    up.num_chunks = kept;
    up.current = -1;
    up.used = 0;
}

static void upload_shutdown(void)
{
    // called with the GPU idle
    if (up.open)
        ID3D12GraphicsCommandList_Close(up.open->cmd);
    for (int i = 0; i < up.num_lists; i++) {
        D3D12_RELEASE(up.lists[i].cmd);
        D3D12_RELEASE(up.lists[i].allocator);
    }
    for (int i = 0; i < up.num_chunks; i++)
        D3D12_RELEASE(up.chunks[i].buffer);
    memset(&up, 0, sizeof(up));
    up.current = -1;
}

/* ------------------------------------------------------------------------- */
/* THE AGILITY SDK                                                            */
/* ------------------------------------------------------------------------- */

/* A device factory: the preview runtime, and the retail one when the process export
   (above) could not be written. It affects only the devices made through it - but
   NGX refuses those. setup/fetch_agility_sdk.py stages D3D12/ and D3D12-preview/ next
   to the exe. */
static ID3D12DeviceFactory *create_device_factory(bool preview, UINT *version)
{
    if (!pD3D12GetInterface) {
        Com_WPrintf("D3D12: this Windows cannot load an Agility SDK runtime; using the system D3D12\n");
        return NULL;
    }

    ID3D12SDKConfiguration1 *config = NULL;
    if (FAILED(pD3D12GetInterface(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&config))) {
        Com_WPrintf("D3D12: no ID3D12SDKConfiguration1 (Windows too old?); using the system D3D12\n");
        return NULL;
    }

    char path[MAX_OSPATH];
    if (!exe_dir_path(preview ? "D3D12-preview\\" : "D3D12\\", path, sizeof(path))) {
        ID3D12SDKConfiguration1_Release(config);
        return NULL;
    }

    UINT want = D3D12_SDK_VERSION;
#ifdef D3D12_PREVIEW_SDK_VERSION
    if (preview)
        want = D3D12_PREVIEW_SDK_VERSION;
#else
    if (preview) {
        Com_WPrintf("D3D12: built without the preview SDK headers; d3d12_agility_preview ignored\n");
        preview = false;
    }
#endif

    ID3D12DeviceFactory *factory = NULL;
    HRESULT hr = ID3D12SDKConfiguration1_CreateDeviceFactory(config, want, path, &IID_ID3D12DeviceFactory,
                                                            (void **)&factory);
    ID3D12SDKConfiguration1_Release(config);
    if (FAILED(hr)) {
        Com_WPrintf("D3D12: no Agility SDK %u in %s (0x%08lx)%s; run setup/fetch_agility_sdk.py\n", want, path,
                    (unsigned long)hr, preview ? " - a preview runtime also needs Windows Developer Mode" : "");
        return NULL;
    }

    if (preview) {
        // the preview's new features (SM 6.10, clustered geometry) sit behind this
        HRESULT ehr = ID3D12DeviceFactory_EnableExperimentalFeatures(factory, 1, &D3D12ExperimentalShaderModels,
                                                                     NULL, NULL);
        if (FAILED(ehr))
            Com_WPrintf("D3D12: experimental shader models refused (0x%08lx) - Developer Mode off?\n",
                        (unsigned long)ehr);
    }

    *version = want;
    return factory;
}

static void enable_debug_layer(int level)
{
    ID3D12Debug1 *debug = NULL;
    HRESULT hr = E_NOINTERFACE;
    if (d3d.device_factory)
        hr = ID3D12DeviceFactory_GetConfigurationInterface(d3d.device_factory, &CLSID_D3D12Debug, &IID_ID3D12Debug1,
                                                           (void **)&debug);
    else if (pD3D12GetDebugInterface)
        hr = pD3D12GetDebugInterface(&IID_ID3D12Debug1, (void **)&debug);
    if (FAILED(hr) || !debug) {
        Com_WPrintf("D3D12: no debug layer (0x%08lx) - d3d12SDKLayers.dll missing?\n", (unsigned long)hr);
        return;
    }
    ID3D12Debug1_EnableDebugLayer(debug);
    if (level >= 2)
        ID3D12Debug1_SetEnableGPUBasedValidation(debug, TRUE);
    ID3D12Debug1_Release(debug);
    Com_Printf("D3D12: debug layer on%s\n", level >= 2 ? ", GPU-based validation on" : "");
}

/* GPU-based validation's 1358 ("Incompatible texture barrier layout ... COMMON ... UAV")
   on the SYSTEM runtime (the DLSS device, see d3d12_device_init): Windows 26100's layer
   flags the path tracer's first read of ASVGF_RNG_SEED in a command list, a
   simultaneous-access texture still in COMMON - which the D3D12 rules allow, and the
   1.620 Agility layer passes the identical command stream. Legacy barriers make no
   difference. Left in, it is an error a frame. tests/d3d12_world_test.c counts it
   apart under `system`. */
#define D3D12_SYSTEM_GBV_COMMON_UAV 1358

static void __stdcall debug_message(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
                                    D3D12_MESSAGE_ID id, LPCSTR description, void *context)
{
    (void)category; (void)context;
    if (id == D3D12_SYSTEM_GBV_COMMON_UAV && !d3d.caps.sdk_version)
        return;
    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        Com_EPrintf("D3D12 [%d]: %s\n", (int)id, description);
    else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
        Com_WPrintf("D3D12 [%d]: %s\n", (int)id, description);
}

static void hook_debug_messages(void)
{
    ID3D12InfoQueue1 *iq = NULL;
    if (FAILED(ID3D12Device5_QueryInterface(d3d.device, &IID_ID3D12InfoQueue1, (void **)&iq)) || !iq)
        return;
    DWORD cookie = 0;
    ID3D12InfoQueue1_RegisterMessageCallback(iq, debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    ID3D12InfoQueue1_Release(iq);
}

/* ------------------------------------------------------------------------- */
/* CAPABILITIES                                                               */
/* ------------------------------------------------------------------------- */

// With an Agility runtime, CheckFeatureSupport1 answers as the SDK version we
// asked for (so a newer runtime cannot report a struct field our headers do not
// know). The old call still works, but the debug layer logs warning 989 for
// every query.
static ID3D12Device16 *caps_device16;

static bool check_feature(D3D12_FEATURE id, void *data, UINT size)
{
    if (caps_device16)
        return SUCCEEDED(ID3D12Device16_CheckFeatureSupport1(caps_device16, id, data, size, d3d.caps.sdk_version));
    return SUCCEEDED(ID3D12Device5_CheckFeatureSupport(d3d.device, id, data, size));
}

#define FEATURE(id, var) check_feature(id, &var, sizeof(var))

static void query_caps(void)
{
    d3d12_caps_t *c = &d3d.caps;

    caps_device16 = NULL;
    if (c->sdk_version &&
        (FAILED(ID3D12Device5_QueryInterface(d3d.device, &IID_ID3D12Device16, (void **)&caps_device16))))
        caps_device16 = NULL;

    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { 0 };
    fl.NumFeatureLevels = ARRAYSIZE(levels);
    fl.pFeatureLevelsRequested = levels;
    if (FEATURE(D3D12_FEATURE_FEATURE_LEVELS, fl))
        c->feature_level = fl.MaxSupportedFeatureLevel;

    // the highest shader model this runtime knows, walked down until accepted
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { 0 };
    c->shader_model = 0;
    for (int m = D3D_HIGHEST_SHADER_MODEL; m >= D3D_SHADER_MODEL_6_0; m--) {
        sm.HighestShaderModel = (D3D_SHADER_MODEL)m;
        if (FEATURE(D3D12_FEATURE_SHADER_MODEL, sm)) {
            c->shader_model = sm.HighestShaderModel;
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o0 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS, o0))
        c->binding_tier = o0.ResourceBindingTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS5, o5))
        c->raytracing_tier = o5.RaytracingTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS6, o6))
        c->vrs_tier = o6.VariableShadingRateTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS7, o7)) {
        c->mesh_shader_tier = o7.MeshShaderTier;
        c->sampler_feedback_tier = o7.SamplerFeedbackTier;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS9, o9))
        c->wave_mma_tier = o9.WaveMMATier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS12, o12))
        c->enhanced_barriers = o12.EnhancedBarriersSupported != FALSE;
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS16, o16))
        c->gpu_upload_heap = o16.GPUUploadHeapSupported != FALSE;
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS21, o21))
        c->work_graphs_tier = o21.WorkGraphsTier;
#ifdef D3D12_PREVIEW_SDK_VERSION
    // preview runtime only: the retail one refuses both queries
    D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2 rt2 = { 0 };
    if (FEATURE(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2, rt2))
        c->clusters_ptlas = rt2.ClustersAndPTLASSupported != FALSE;
    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT la = { 0 };
    if (FEATURE(D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT, la))
        c->linear_algebra_tier = (int)la.LinearAlgebraTier;
#endif

    if (caps_device16) {
        ID3D12Device16_Release(caps_device16);
        caps_device16 = NULL;
    }

    BOOL tearing = FALSE;
    if (SUCCEEDED(IDXGIFactory6_CheckFeatureSupport(d3d.factory, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &tearing, sizeof(tearing))))
        c->tearing = tearing != FALSE;
}

/* The DirectX 12 Ultimate feature set, checked feature by feature rather than
   trusting the feature level alone. */
bool d3d12_is_ultimate(void)
{
    const d3d12_caps_t *c = &d3d.caps;
    return c->feature_level >= D3D_FEATURE_LEVEL_12_2 &&
           c->raytracing_tier >= D3D12_RAYTRACING_TIER_1_1 &&
           c->mesh_shader_tier >= D3D12_MESH_SHADER_TIER_1 &&
           c->vrs_tier >= D3D12_VARIABLE_SHADING_RATE_TIER_2 &&
           c->sampler_feedback_tier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9 &&
           c->shader_model >= D3D_SHADER_MODEL_6_5;
}

/* D3D12's tier enums do not share an encoding: most are 10 * major + minor
   (ray tracing 1.1 = 11), sampler feedback is 100 * (0.9 = 90), linear algebra
   is 16 * (1.0 = 0x10). `scale` is the value of one major version. */
static const char *tier_name(int tier, int scale)
{
    static char buf[8][16];
    static int which;
    if (!tier)
        return "no";
    char *b = buf[which++ & 7];
    Q_snprintf(b, sizeof(buf[0]), "%d.%d", tier / scale, (tier % scale) * 10 / scale);
    return b;
}

void d3d12_print_info(void)
{
    const d3d12_caps_t *c = &d3d.caps;
    if (!d3d.device) {
        Com_Printf("The D3D12 renderer is not running (vid_rtx 3).\n");
        return;
    }

    Com_Printf("---- D3D12 ----\n");
    Com_Printf("adapter           %s, %llu MB\n", c->adapter_name, (unsigned long long)(c->dedicated_vram >> 20));
    Com_Printf("driver            %u.%u.%u.%u\n",
               (unsigned)(c->driver_version >> 48), (unsigned)(c->driver_version >> 32) & 0xffff,
               (unsigned)(c->driver_version >> 16) & 0xffff, (unsigned)c->driver_version & 0xffff);
    if (c->sdk_version)
        Com_Printf("runtime           Agility SDK %u%s\n", c->sdk_version,
                   c->preview_sdk ? " (preview)" : c->process_sdk ? " (the process's, D3D12SDKVersion)" : " (device factory)");
    else
        Com_Printf("runtime           system D3D12\n");
    // D3D_FEATURE_LEVEL_12_2 is 0xc200: major in the top nibble, minor in the next
    Com_Printf("feature level     %d.%d\n", c->feature_level >> 12, (c->feature_level >> 8) & 0xf);
    Com_Printf("DX12 Ultimate     %s\n", d3d12_is_ultimate() ? "yes - the Ultimate path" : "no - the base path");
    Com_Printf("shader model      %d.%d\n", c->shader_model >> 4, c->shader_model & 0xf);
    Com_Printf("ray tracing       %s%s\n", tier_name(c->raytracing_tier, 10),
               c->raytracing_tier >= D3D12_RAYTRACING_TIER_2_0 ? " (DXR 2.0)" :
               c->raytracing_tier >= D3D12_RAYTRACING_TIER_1_2 ? " (DXR 1.2: shader execution reordering, opacity micromaps)" :
               c->raytracing_tier >= D3D12_RAYTRACING_TIER_1_1 ? " (DXR 1.1: ray query)" : "");
    Com_Printf("clusters + PTLAS  %s%s\n", c->clusters_ptlas ? "yes" : "no",
               c->clusters_ptlas ? " (clustered geometry, partitioned TLAS)" :
               c->preview_sdk ? "" : " (needs d3d12_agility_preview 1 + Developer Mode)");
    Com_Printf("linear algebra    %s%s\n", tier_name(c->linear_algebra_tier, 16),
               c->linear_algebra_tier ? " (cooperative vectors)" : "");
    Com_Printf("resource binding  tier %d\n", (int)c->binding_tier);
    Com_Printf("mesh shaders      %s\n", tier_name(c->mesh_shader_tier, 10));
    Com_Printf("variable rate     tier %d\n", (int)c->vrs_tier);
    Com_Printf("work graphs       %s\n", tier_name(c->work_graphs_tier, 10));
    Com_Printf("sampler feedback  %s\n", tier_name(c->sampler_feedback_tier, 100));
    Com_Printf("wave MMA          %s\n", tier_name(c->wave_mma_tier, 10));
    Com_Printf("enhanced barriers %s\n", c->enhanced_barriers ? "yes" : "no");
    Com_Printf("GPU upload heaps  %s\n", c->gpu_upload_heap ? "yes" : "no");
    Com_Printf("tearing           %s\n", c->tearing ? "yes" : "no");
}

/* ------------------------------------------------------------------------- */
/* INIT / SHUTDOWN                                                            */
/* ------------------------------------------------------------------------- */

/* DIRECTX 12 ULTIMATE WHERE THE GPU HAS IT, THE BASE PATH WHERE IT DOES NOT.

   The minimum asked for here is 12_0, but that is only a floor: the device a
   12_2 GPU gives back IS a 12_2 device, with every Ultimate feature live.
   d3d12_is_ultimate() then says which path the renderer takes, and each
   Ultimate-or-newer feature (mesh shaders, VRS, sampler feedback, DXR 1.2
   SER/OMM, clusters + PTLAS) is switched on by its own cap, with the base path
   as the fallback.

   What the path tracer cannot do without is DXR 1.1 ray query - part of
   Ultimate, but also exposed below it (GTX 10/16 through the driver), which is
   why this is not a 12_2 floor. */
#define D3D12_REQUIRED_FEATURE_LEVEL D3D_FEATURE_LEVEL_12_0

static HRESULT create_device(IDXGIAdapter4 *adapter)
{
    if (d3d.device_factory)
        return ID3D12DeviceFactory_CreateDevice(d3d.device_factory, (IUnknown *)adapter, D3D12_REQUIRED_FEATURE_LEVEL,
                                                &IID_ID3D12Device5, (void **)&d3d.device);
    return pD3D12CreateDevice((IUnknown *)adapter, D3D12_REQUIRED_FEATURE_LEVEL, &IID_ID3D12Device5,
                              (void **)&d3d.device);
}

static bool pick_adapter_and_create_device(void)
{
    int want = d3d12_adapter->integer;
    int hw_index = 0;

    for (UINT i = 0;; i++) {
        IDXGIAdapter4 *adapter = NULL;
        if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(d3d.factory, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                            &IID_IDXGIAdapter4, (void **)&adapter)))
            break;

        DXGI_ADAPTER_DESC3 desc;
        IDXGIAdapter4_GetDesc3(adapter, &desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) {
            IDXGIAdapter4_Release(adapter);
            continue;
        }
        if (want >= 0 && hw_index++ != want) {
            IDXGIAdapter4_Release(adapter);
            continue;
        }

        HRESULT hr = create_device(adapter);
        if (FAILED(hr)) {
            Com_WPrintf("D3D12: adapter %u cannot make a D3D12 device (0x%08lx)\n", i,
                        (unsigned long)hr);
            IDXGIAdapter4_Release(adapter);
            if (want >= 0)
                break;
            continue;
        }

        d3d.adapter = adapter;
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, d3d.caps.adapter_name,
                            sizeof(d3d.caps.adapter_name), NULL, NULL);
        d3d.caps.adapter_luid = desc.AdapterLuid;
        d3d.caps.dedicated_vram = desc.DedicatedVideoMemory;
        LARGE_INTEGER umd;
        if (SUCCEEDED(IDXGIAdapter4_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd)))
            d3d.caps.driver_version = (uint64_t)umd.QuadPart;
        return true;
    }

    Com_EPrintf("D3D12: no hardware adapter can run a D3D12 device%s\n", want >= 0 ? " (check d3d12_adapter)" : "");
    return false;
}

static bool create_heap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shader_visible, ID3D12DescriptorHeap **heap,
                        const char *name)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd = { 0 };
    hd.Type = type;
    hd.NumDescriptors = count;
    hd.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!D3D_CHECK(ID3D12Device5_CreateDescriptorHeap(d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void **)heap)))
        return false;
    d3d12_set_name((ID3D12Object *)*heap, name);
    return true;
}

static void bindings_print(const char *text)
{
    Com_EPrintf("D3D12: %s\n", text);
}

/* The binding layer (bindings.c) is kept free of engine headers for the
   headless tests, so it is told here where the heaps are. */
static bool init_bindings(void)
{
    d3d12_bindings_print = bindings_print;

    memset(&d3d12_heaps, 0, sizeof(d3d12_heaps));
    d3d12_heaps.device = (ID3D12Device *)d3d.device;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d3d.srv_heap, &d3d12_heaps.resource_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d3d.srv_heap, &d3d12_heaps.resource_gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d3d.sampler_heap, &d3d12_heaps.sampler_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d3d.sampler_heap, &d3d12_heaps.sampler_gpu);
    d3d12_heaps.resource_increment = d3d.srv_increment;
    d3d12_heaps.sampler_increment = d3d.sampler_increment;
    d3d12_heaps.resource_next = D3D12_HEAP_SETS_BASE;
    d3d12_heaps.resource_end = D3D12_HEAP_SETS_BASE + D3D12_HEAP_SETS_COUNT;
    d3d12_heaps.sampler_next = D3D12_SAMPLER_HEAP_SETS_BASE;
    d3d12_heaps.sampler_end = D3D12_SAMPLER_HEAP_SIZE;

    char error[1024] = "";
    if (FAILED(d3d12_bindings_create_root_signatures((ID3D12Device *)d3d.device, pD3D12SerializeVersionedRootSignature,
                                                     error, sizeof(error)))) {
        Com_EPrintf("D3D12: the path tracer's root signatures: %s\n", error);
        return false;
    }
    Com_DPrintf("D3D12: %d root signatures\n", D3D12_NUM_PIPELINE_LAYOUTS);
    return true;
}

bool d3d12_device_init(void)
{
    memset(&d3d, 0, sizeof(d3d));

    d3d12_debug = Cvar_Get("d3d12_debug", "0", CVAR_REFRESH);
    d3d12_dred = Cvar_Get("d3d12_dred", "1", CVAR_REFRESH);
    d3d12_agility_preview = Cvar_Get("d3d12_agility_preview", "0", CVAR_REFRESH | CVAR_ARCHIVE);
    d3d12_adapter = Cvar_Get("d3d12_adapter", "-1", CVAR_REFRESH | CVAR_ARCHIVE);

    // hidden-but-set is off: the menu row shows only where the preview can start
    const bool preview_available = preview_runtime_available();
    Cvar_FullSet("d3d12_agility_preview_available", preview_available ? "1" : "0", CVAR_ROM, FROM_CODE);
    bool preview = d3d12_agility_preview->integer != 0 && preview_available;
    if (d3d12_agility_preview->integer && !preview_available)
        Com_WPrintf("D3D12: d3d12_agility_preview 1, but the preview runtime cannot start here (%s) - the retail one\n",
                    windows_developer_mode() ? "no D3D12-preview runtime beside the exe" : "Windows Developer Mode is off");
    if (!load_dlls(!preview))
        goto fail;

    /* DLSS AND THE AGILITY SDK. NGX will not start on a device made through a device
       factory: NVSDK_NGX_D3D12_Init_with_ProjectID fails with FAIL_PlatformError
       ("NGXGetPath failed to obtain path to models") - with or without the debug layer,
       wherever the runtime's folder is, and after ApplyToGlobalState as well. The same
       1.620 runtime taken through a D3D12SDKVersion export from the exe works, and so
       does the system runtime (probed 2026-11-06). So:
       - retail, with the export written (load_dlls): the device is made on the
         process's runtime, 1.620, and DLSS and SER both have what they need;
       - retail without it: a factory device, or with pt_dlss on the system runtime
         (frontend.c restarts the renderer when pt_dlss is turned on over a factory
         device);
       - the preview, there for clustered geometry, keeps its factory and goes without
         DLSS. */
    UINT version = 0;
    if (!preview && process_agility)
        ;   // D3D12CreateDevice: the process's runtime
    else if (!preview && Cvar_VariableInteger("pt_dlss"))
        Com_Printf("D3D12: the system runtime, for DLSS (NGX does not start on an Agility SDK factory's device)\n");
    else
        d3d.device_factory = create_device_factory(preview, &version);
    if (!d3d.device_factory && preview) {
        Com_WPrintf("D3D12: falling back to the retail Agility SDK\n");
        preview = false;
        if (!process_agility)
            d3d.device_factory = create_device_factory(false, &version);
    }
    d3d.caps.sdk_version = d3d.device_factory ? version : 0;
    d3d.caps.preview_sdk = d3d.device_factory && preview;

    if (d3d12_debug->integer)
        enable_debug_layer(d3d12_debug->integer);
    set_dred(d3d12_dred->integer);

    if (!D3D_CHECK(pCreateDXGIFactory2(d3d12_debug->integer ? DXGI_CREATE_FACTORY_DEBUG : 0, &IID_IDXGIFactory6,
                                       (void **)&d3d.factory)))
        goto fail;

    if (!pick_adapter_and_create_device())
        goto fail;
    d3d12_set_name((ID3D12Object *)d3d.device, "Q2RTX D3D12 device");

    // the export took: D3D12Core.dll is ours (a runtime that refused it would be the system's)
    if (!d3d.device_factory && process_agility) {
        if (process_runtime_is_agility()) {
            d3d.caps.sdk_version = D3D12_SDK_VERSION;
            d3d.caps.process_sdk = true;
        } else {
            Com_WPrintf("D3D12: the D3D12SDKVersion export did not take; on the system runtime\n");
        }
    }

    if (d3d12_debug->integer)
        hook_debug_messages();

    query_caps();

    D3D12_COMMAND_QUEUE_DESC qd = { 0 };
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!D3D_CHECK(ID3D12Device5_CreateCommandQueue(d3d.device, &qd, &IID_ID3D12CommandQueue, (void **)&d3d.queue)))
        goto fail;
    d3d12_set_name((ID3D12Object *)d3d.queue, "direct queue");

    for (int i = 0; i < D3D12_FRAMES_IN_FLIGHT; i++) {
        if (!D3D_CHECK(ID3D12Device5_CreateCommandAllocator(d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            &IID_ID3D12CommandAllocator,
                                                            (void **)&d3d.frames[i].allocator)))
            goto fail;
    }
    // created closed: R_BeginFrame resets it onto the frame's allocator
    if (!D3D_CHECK(ID3D12Device5_CreateCommandList1(d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    D3D12_COMMAND_LIST_FLAG_NONE, &IID_ID3D12GraphicsCommandList4,
                                                    (void **)&d3d.cmd)))
        goto fail;
    d3d12_set_name((ID3D12Object *)d3d.cmd, "frame");

    if (!D3D_CHECK(ID3D12Device5_CreateFence(d3d.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                             (void **)&d3d.fence)))
        goto fail;
    d3d.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!d3d.fence_event)
        goto fail;

    if (!create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_HEAP_SIZE, true, &d3d.srv_heap, "CBV/SRV/UAV heap") ||
        !create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, D3D12_SAMPLER_HEAP_SIZE, true, &d3d.sampler_heap, "samplers") ||
        !create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_NUM_RTVS, false, &d3d.rtv_heap, "RTVs"))
        goto fail;
    d3d.srv_increment = ID3D12Device5_GetDescriptorHandleIncrementSize(d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d3d.sampler_increment = ID3D12Device5_GetDescriptorHandleIncrementSize(d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    d3d.rtv_increment = ID3D12Device5_GetDescriptorHandleIncrementSize(d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // the binding layer: where set instances go, and every pass's root signature
    if (!init_bindings())
        goto fail;

    if (d3d.caps.sdk_version)
        Com_Printf("D3D12: %s, Agility SDK %u%s, shader model %d.%d, ray tracing tier %s, %s\n", d3d.caps.adapter_name,
                   d3d.caps.sdk_version, d3d.caps.preview_sdk ? " preview" : d3d.caps.process_sdk ? " (process)" : "",
                   d3d.caps.shader_model >> 4,
                   d3d.caps.shader_model & 0xf, tier_name(d3d.caps.raytracing_tier, 10),
                   d3d12_is_ultimate() ? "DirectX 12 Ultimate" : "not DirectX 12 Ultimate (base path)");
    else
        Com_Printf("D3D12: %s, system runtime, shader model %d.%d, ray tracing tier %s, %s\n", d3d.caps.adapter_name,
                   d3d.caps.shader_model >> 4, d3d.caps.shader_model & 0xf, tier_name(d3d.caps.raytracing_tier, 10),
                   d3d12_is_ultimate() ? "DirectX 12 Ultimate" : "not DirectX 12 Ultimate (base path)");
    if (d3d.caps.raytracing_tier < D3D12_RAYTRACING_TIER_1_1)
        Com_WPrintf("D3D12: this GPU has no DXR 1.1 ray query; the path tracer will not run on it\n");

    /* Every shader is built for shader model 6.6 (D3D12_SHADER_MODEL in
       cmake/compileShadersD3D12.cmake), the 2D ones included, so below it not one
       pipeline would be made. Windows 11's own runtime has 6.6; Windows 10's stops
       at 6.5 and needs the Agility SDK runtime, which the release does not ship
       (a closed-source Microsoft DLL beside a GPL program - the same rule as the
       DLSS DLLs). Failing here sends refresh.c back to Vulkan with a reason. */
    if (d3d.caps.shader_model < D3D_SHADER_MODEL_6_6) {
        Com_EPrintf("D3D12: shader model %d.%d - the renderer needs 6.6. Windows 11 has it; on Windows 10 put "
                    "D3D12Core.dll from Microsoft's Agility SDK 1.620.1 in D3D12\\ beside q2rtx.exe (README-FIRST.txt)\n",
                    d3d.caps.shader_model >> 4, d3d.caps.shader_model & 0xf);
        goto fail;
    }

    d3d.initialized = true;
    return true;

fail:
    d3d12_device_shutdown();
    return false;
}

void d3d12_device_shutdown(void)
{
    d3d12_wait_idle();
    upload_shutdown();
    d3d12_collect_garbage(true);

    d3d12_bindings_destroy_root_signatures();
    memset(&d3d12_heaps, 0, sizeof(d3d12_heaps));
    D3D12_RELEASE(d3d.rtv_heap);
    D3D12_RELEASE(d3d.sampler_heap);
    D3D12_RELEASE(d3d.srv_heap);
    if (d3d.fence_event)
        CloseHandle(d3d.fence_event);
    D3D12_RELEASE(d3d.fence);
    D3D12_RELEASE(d3d.cmd);
    for (int i = 0; i < D3D12_FRAMES_IN_FLIGHT; i++)
        D3D12_RELEASE(d3d.frames[i].allocator);
    D3D12_RELEASE(d3d.queue);

    if (d3d.device && d3d12_debug && d3d12_debug->integer) {
        // whatever is still alive now is a leak
        ID3D12DebugDevice *dd = NULL;
        if (SUCCEEDED(ID3D12Device5_QueryInterface(d3d.device, &IID_ID3D12DebugDevice, (void **)&dd)) && dd) {
            ID3D12DebugDevice_ReportLiveDeviceObjects(dd, D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
            ID3D12DebugDevice_Release(dd);
        }
    }

    D3D12_RELEASE(d3d.device);
    D3D12_RELEASE(d3d.adapter);
    D3D12_RELEASE(d3d.factory);
    D3D12_RELEASE(d3d.device_factory);
    // d3d12.dll and dxgi.dll stay loaded: NGX, overlays and the debug layer
    // keep pointers into them, and unloading under those has crashed engines
    memset(&d3d, 0, sizeof(d3d));
}
