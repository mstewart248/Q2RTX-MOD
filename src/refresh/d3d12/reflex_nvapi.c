/*
NVIDIA REFLEX for the D3D12 renderer (vid_rtx 3), through NVAPI.

vkpt/reflex.c is Reflex on Vulkan (VK_NV_low_latency2). On D3D12 the same driver
feature is NVAPI's: NvAPI_D3D_SetSleepMode / Sleep / SetLatencyMarker / GetLatency
on the device, and NvAPI_D3D12_NotifyOutOfBandCommandQueue for frame generation's
present queue. As reflex.c's VRR query does, nvapi64.dll is loaded at run time and
the entry points come from nvapi_QueryInterface by their public ids
(nvapi_interface.h); the structs are nvapi.h's, declared by hand - the SDK is not
in the tree.

Everything that is policy is reflex.c's, called, not copied:
    Reflex_RegisterCvars    pt_reflex (0 off, 1 on, 2 + boost), pt_reflex_fps_max,
                            pt_reflex_stats, pt_vrr, pt_reflex_vrr_cap,
                            pt_dlss_fg_pace_to_refresh
    Reflex_ModeToApply      the frame interval from the fps cap, frame generation's
                            pacing and the VRR cap; Reflex_ModeApplied its console line
    Reflex_PollVRR          NVAPI's VRR answer, asked again once a second
    Reflex_PrintTimings     the pt_reflex_stats line

THE FRAME, with vkpt's marker placement:
    R_LatencySleep      d3d12_reflex_sleep: the mode, a new frame id, NvAPI_D3D_Sleep
                        with pt_reflex on, then INPUT_SAMPLE and SIMULATION_START -
                        the client samples input right after
    R_EndFrame          SIMULATION_END and RENDERSUBMIT_START at its top,
                        RENDERSUBMIT_END once the frame's list is executed
    the present         PRESENT_START / PRESENT_END around d3d12_present - or, with
                        frame generation, around the real frame's present on
                        fg_present.c's thread, by the frame id its item carries
                        (frontend.c). Generated frames carry none.
Frame generation's present queue is told it is out of band (OUT_OF_BAND_PRESENT,
once, when swapchain.c makes it), as vkpt tells its present queue: the generated
presents are not work the game simulated.

The markers flow with pt_reflex 0 too, so pt_reflex_stats can measure the baseline.
*/

#include "d3d12pt.h"
#include "nvapi_rt.h"
#include "../vkpt/reflex.h"

/* ---- nvapi.h, by hand ---- */

// MAKE_NVAPI_VERSION(type, 1)
#define NV_VERSION1(s)  ((uint32_t)sizeof(s) | (1u << 16))

// NV_SET_SLEEP_MODE_PARAMS_V1 (NvBool is an NvU8)
typedef struct {
    uint32_t version;
    uint8_t  bLowLatencyMode;
    uint8_t  bLowLatencyBoost;
    uint32_t minimumIntervalUs;
    uint8_t  bUseMarkersToOptimize;
    uint8_t  bUseMinQueueTime;
    uint8_t  rsvd[30];
} nv_sleep_mode_params_t;

// NV_LATENCY_MARKER_PARAMS_V1
typedef struct {
    uint32_t version;
    uint64_t frameID;
    int      markerType;        // NV_LATENCY_MARKER_TYPE: d3d12_reflex_marker_t's values
    uint64_t rsvd0;
    uint8_t  rsvd[56];
} nv_marker_params_t;

// NV_LATENCY_RESULT_PARAMS_V1's FrameReport; the times are in microseconds
typedef struct {
    uint64_t frameID, inputSampleTime, simStartTime, simEndTime;
    uint64_t renderSubmitStartTime, renderSubmitEndTime, presentStartTime, presentEndTime;
    uint64_t driverStartTime, driverEndTime, osRenderQueueStartTime, osRenderQueueEndTime;
    uint64_t gpuRenderStartTime, gpuRenderEndTime;
    uint32_t gpuActiveRenderTimeUs, gpuFrameTimeUs;
    uint64_t cameraConstructedTime;
    uint32_t crossAdapterCopyTimeUs, aiFrameTimeUs;
    uint8_t  rsvd[104];
} nv_frame_report_t;

// NV_LATENCY_RESULT_PARAMS_V1: the driver's last 64 frames, the newest last
typedef struct {
    uint32_t          version;
    nv_frame_report_t frameReport[64];
    uint8_t           rsvd[32];
} nv_latency_result_t;

// the sizes nvapi.h's layout gives (its #pragma pack(8) changes nothing here); the
// version word carries the size, so a wrong one is refused by the driver
typedef char nv_sleep_mode_size_check[sizeof(nv_sleep_mode_params_t) == 44 ? 1 : -1];
typedef char nv_marker_size_check[sizeof(nv_marker_params_t) == 88 ? 1 : -1];
typedef char nv_frame_report_size_check[sizeof(nv_frame_report_t) == 240 ? 1 : -1];
typedef char nv_latency_result_size_check[sizeof(nv_latency_result_t) == 15400 ? 1 : -1];

// NV_OUT_OF_BAND_CQ_TYPE's OUT_OF_BAND_PRESENT
#define NV_OUT_OF_BAND_PRESENT  1

typedef int (*PFN_NvAPI_D3D_SetSleepMode)(IUnknown *device, nv_sleep_mode_params_t *params);
typedef int (*PFN_NvAPI_D3D_Sleep)(IUnknown *device);
typedef int (*PFN_NvAPI_D3D_SetLatencyMarker)(IUnknown *device, nv_marker_params_t *params);
typedef int (*PFN_NvAPI_D3D_GetLatency)(IUnknown *device, nv_latency_result_t *params);
typedef int (*PFN_NvAPI_D3D12_NotifyOutOfBandCommandQueue)(ID3D12CommandQueue *queue, int type);

static struct {
    bool                                        loaded;     // nvapi64.dll looked for
    PFN_NvAPI_D3D_SetSleepMode                  set_sleep_mode;
    PFN_NvAPI_D3D_Sleep                         sleep;
    PFN_NvAPI_D3D_SetLatencyMarker              set_marker;
    PFN_NvAPI_D3D_GetLatency                    get_latency;
    PFN_NvAPI_D3D12_NotifyOutOfBandCommandQueue notify_queue;

    cvar_t      *reflex;        // pt_reflex
    bool        available;      // this device took a sleep mode
    bool        mode_applied;
    uint64_t    frame_id;       // the frame being built; 0 before the first sleep
    bool        submitted;      // a frame went to the present since the last sleep
} rx;

static void load_nvapi(void)
{
    if (rx.loaded)
        return;
    rx.loaded = true;

    // nvapi_interface.h's ids, through nvapi_rt.c's loader (NULL without NVAPI)
    rx.set_sleep_mode = (PFN_NvAPI_D3D_SetSleepMode)d3d12_nvapi_query(0xac1ca9e0u);
    rx.sleep          = (PFN_NvAPI_D3D_Sleep)d3d12_nvapi_query(0x852cd1d2u);
    rx.set_marker     = (PFN_NvAPI_D3D_SetLatencyMarker)d3d12_nvapi_query(0xd9984c05u);
    rx.get_latency    = (PFN_NvAPI_D3D_GetLatency)d3d12_nvapi_query(0x1a587f9cu);
    rx.notify_queue   = (PFN_NvAPI_D3D12_NotifyOutOfBandCommandQueue)d3d12_nvapi_query(0x03d6e8cbu);
}

static int set_sleep_mode(const reflex_mode_t *m)
{
    nv_sleep_mode_params_t p;
    memset(&p, 0, sizeof(p));
    p.version = NV_VERSION1(p);
    p.bLowLatencyMode = m->mode != 0;
    p.bLowLatencyBoost = m->mode >= 2;
    p.minimumIntervalUs = m->interval_us;
    return rx.set_sleep_mode((IUnknown *)d3d.device, &p);
}

// vkpt's reflex_apply_mode: checked every frame, sent when it changed - pt_reflex 0
// included, so switching it off tells the driver to stop rather than only skipping
// the sleep
static void apply_mode(void)
{
    reflex_mode_t m;
    if (!Reflex_ModeToApply(&m, rx.mode_applied))
        return;

    const int status = set_sleep_mode(&m);
    if (status != 0) {
        // asked again every frame it would print every frame: off instead
        Com_EPrintf("Reflex: NvAPI_D3D_SetSleepMode failed (%d), Reflex is off until the renderer "
                    "restarts\n", status);
        rx.available = false;
        Cvar_FullSet("pt_reflex_available", "0", CVAR_ROM, FROM_CODE);
        return;
    }
    Reflex_ModeApplied(&m);
    rx.mode_applied = true;
}

// pt_reflex_stats, once a second: the newest frame the driver has timed
static void report_timings(void)
{
    if (!rx.get_latency || !Reflex_TimingsDue())
        return;

    static nv_latency_result_t result;     // 15 KB
    memset(&result, 0, sizeof(result));
    result.version = NV_VERSION1(result);
    if (rx.get_latency((IUnknown *)d3d.device, &result) != 0)
        return;

    const nv_frame_report_t *r = &result.frameReport[63];
    const reflex_timings_t t = {
        .input_sample  = r->inputSampleTime,
        .sim_start     = r->simStartTime,
        .sim_end       = r->simEndTime,
        .submit_start  = r->renderSubmitStartTime,
        .submit_end    = r->renderSubmitEndTime,
        .gpu_start     = r->gpuRenderStartTime,
        .gpu_end       = r->gpuRenderEndTime,
        .present_start = r->presentStartTime,
        .present_end   = r->presentEndTime,
    };
    Reflex_PrintTimings(&t);
}

void d3d12_reflex_init(void)
{
    Reflex_RegisterCvars();     // pt_reflex_available 0 until this device takes a mode
    rx.reflex = Cvar_Get("pt_reflex", "1", CVAR_ARCHIVE);
    rx.available = false;
    rx.mode_applied = false;
    rx.submitted = false;
    rx.frame_id = 0;

    // the window's monitor's VRR answer, which the mode's VRR cap reads
    d3d12_frontend_detect_vrr();

    load_nvapi();
    if (!rx.set_sleep_mode || !rx.sleep || !rx.set_marker) {
        Com_Printf("Reflex: NVAPI's D3D low latency entry points are not available, low latency "
                   "mode disabled\n");
        return;
    }

    // another vendor's adapter, or a driver without Reflex, refuses it
    reflex_mode_t m;
    Reflex_ModeToApply(&m, false);
    const int status = set_sleep_mode(&m);
    if (status != 0) {
        Com_Printf("Reflex: NvAPI_D3D_SetSleepMode refused this device (%d), low latency mode "
                   "disabled\n", status);
        return;
    }

    rx.available = true;
    Com_Printf("Reflex: NVAPI low latency available (D3D12)\n");
    Reflex_ModeApplied(&m);
    rx.mode_applied = true;
    Cvar_FullSet("pt_reflex_available", "1", CVAR_ROM, FROM_CODE);
}

void d3d12_reflex_shutdown(void)
{
    rx.available = false;
    rx.mode_applied = false;
    Cvar_FullSet("pt_reflex_available", "0", CVAR_ROM, FROM_CODE);
}

void d3d12_reflex_sleep(void)
{
    // ahead of the mode, which reads the answer for the VRR cap
    Reflex_PollVRR();
    if (!rx.available || !d3d.device)
        return;

    apply_mode();
    if (!rx.available)
        return;
    report_timings();

    // a new frame whether the sleep is on or not: the markers keep timing frames
    rx.frame_id++;

    /* Only after a frame went to the present: with none since the last sleep
       (minimized, a load, a client frame that drew nothing) there is nothing for the
       driver to pace by. */
    if (rx.reflex->integer != 0 && rx.submitted)
        rx.sleep((IUnknown *)d3d.device);
    rx.submitted = false;

    d3d12_reflex_marker(D3D12_REFLEX_INPUT_SAMPLE);
    d3d12_reflex_marker(D3D12_REFLEX_SIMULATION_START);
}

void d3d12_reflex_marker_for_frame(d3d12_reflex_marker_t marker, uint64_t frame_id)
{
    if (!rx.available || !frame_id)
        return;

    nv_marker_params_t p;
    memset(&p, 0, sizeof(p));
    p.version = NV_VERSION1(p);
    p.frameID = frame_id;
    p.markerType = (int)marker;
    rx.set_marker((IUnknown *)d3d.device, &p);
}

void d3d12_reflex_marker(d3d12_reflex_marker_t marker)
{
    if (marker == D3D12_REFLEX_RENDERSUBMIT_END)
        rx.submitted = true;
    d3d12_reflex_marker_for_frame(marker, rx.frame_id);
}

uint64_t d3d12_reflex_frame_id(void)
{
    return rx.available ? rx.frame_id : 0;
}

void d3d12_reflex_out_of_band_present_queue(ID3D12CommandQueue *queue)
{
    if (!rx.available || !rx.notify_queue || !queue)
        return;
    const int status = rx.notify_queue(queue, NV_OUT_OF_BAND_PRESENT);
    if (status != 0)
        Com_DPrintf("Reflex: NvAPI_D3D12_NotifyOutOfBandCommandQueue failed (%d)\n", status);
}
