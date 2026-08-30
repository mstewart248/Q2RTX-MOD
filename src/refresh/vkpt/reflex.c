// Copyright (c) 2026 Matt Stewart
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// NVIDIA Reflex via VK_NV_low_latency2 - see reflex.h for why this needs no SDK.

#include "shared/shared.h"
#include "common/common.h"
#include "common/cvar.h"
#include "system/system.h"
#include <SDL.h>

#include "vkpt.h"
#include "reflex.h"
#include "DLSS.h"
#include "fg_present.h"

static PFN_vkSetLatencySleepModeNV  qvkSetLatencySleepModeNV  = NULL;
static PFN_vkLatencySleepNV         qvkLatencySleepNV         = NULL;
static PFN_vkSetLatencyMarkerNV     qvkSetLatencyMarkerNV     = NULL;
static PFN_vkGetLatencyTimingsNV    qvkGetLatencyTimingsNV    = NULL;
static PFN_vkQueueNotifyOutOfBandNV qvkQueueNotifyOutOfBandNV = NULL;

/* vkLatencySleepNV signals a timeline semaphore rather than blocking, so the caller
   owns the wait. One semaphore for the lifetime of the device, with a monotonically
   increasing value. */
static VkSemaphore   reflex_sleep_semaphore = VK_NULL_HANDLE;
static uint64_t      reflex_sleep_value = 0;

/* Frame identity. presentID must increase by one per frame and is what ties a frame's
   markers to its present. Starts at 1 - zero reads as "unset" to the driver. */
static uint64_t      reflex_present_id = 0;

static VkSwapchainKHR reflex_swapchain = VK_NULL_HANDLE;
static bool          reflex_mode_applied = false;

/* 0 = off, 1 = on, 2 = on + boost. Boost keeps clocks up when the GPU is idle waiting,
   which trades power for a little more latency reduction. */
static cvar_t *cvar_reflex = NULL;
/* Frame rate cap in fps, 0 for none. Reflex can pace the frame start, which is a much
   smoother limiter than sleeping in the main loop. */
static cvar_t *cvar_reflex_fps_max = NULL;
static cvar_t *cvar_reflex_stats = NULL;
/* With frame generation on, cap the RENDER rate to refresh / multiplier so the presented
   rate lands exactly on the display refresh. See reflex_paced_render_fps(). */
static cvar_t *cvar_fg_pace_to_refresh = NULL;

/* Refresh rate of the display the window is on, or 0 if SDL will not say. */
int Reflex_DisplayRefreshHz(void)
{
    if (!qvk.window)
        return 0;

    int display = SDL_GetWindowDisplayIndex(qvk.window);
    if (display < 0)
        return 0;

    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(display, &mode) != 0)
        return 0;

    return mode.refresh_rate;
}

/* The render-rate cap that makes frame generation land exactly on the refresh rate.
   0 when it does not apply.

   WHY: presenting at any rate that is not the refresh rate beats against the display. At
   2x on a 60Hz panel a 28 fps base presents 56/s into a 60Hz sampler, so some frames are
   shown twice and others dropped - it judders however evenly WE space them. Capping the
   base to 30 makes it exactly 60 presented, one unique frame per refresh. Same trick
   Cyberpunk uses, and why its FG looks right on a 60Hz panel when most do not.

   The honest limit: this can only cap the render rate DOWN. If the GPU cannot sustain
   refresh/multiplier anyway, nothing here helps - the answer is then a lower multiplier
   or lighter settings. */
static int reflex_paced_render_fps(void)
{
    if (!cvar_fg_pace_to_refresh || cvar_fg_pace_to_refresh->integer == 0)
        return 0;

    unsigned int mult = DLSSGMultiplier();
    if (mult < 2)
        return 0;

    int refresh = Reflex_DisplayRefreshHz();
    if (refresh <= 0)
        return 0;

    int fps = refresh / (int)mult;
    return (fps > 0) ? fps : 0;
}

void Reflex_Init(void)
{
    cvar_reflex         = Cvar_Get("pt_reflex", "1", CVAR_ARCHIVE);
    cvar_reflex_fps_max = Cvar_Get("pt_reflex_fps_max", "0", CVAR_ARCHIVE);
    cvar_reflex_stats   = Cvar_Get("pt_reflex_stats", "0", 0);
    /* DEFAULT OFF. Capping the render rate to refresh/multiplier is sound in theory -
       it makes the presented rate land exactly on the refresh rate - but MEASURED ON
       MATT'S SETUP IT LOOKED WORSE, so it is opt-in. It throws away real frames the
       GPU was managing (a 40 fps base capped to 30 samples motion less often) and the
       cap itself adds wait. Do not re-enable this by default without testing it. */
    cvar_fg_pace_to_refresh = Cvar_Get("pt_dlss_fg_pace_to_refresh", "0", 0);   /* NOT archived */

    if (!qvk.supports_low_latency) {
        Com_Printf("Reflex: VK_NV_low_latency2 not available, low latency mode disabled\n");
        return;
    }

    qvkSetLatencySleepModeNV  = (PFN_vkSetLatencySleepModeNV)  vkGetDeviceProcAddr(qvk.device, "vkSetLatencySleepModeNV");
    qvkLatencySleepNV         = (PFN_vkLatencySleepNV)         vkGetDeviceProcAddr(qvk.device, "vkLatencySleepNV");
    qvkSetLatencyMarkerNV     = (PFN_vkSetLatencyMarkerNV)     vkGetDeviceProcAddr(qvk.device, "vkSetLatencyMarkerNV");
    qvkGetLatencyTimingsNV    = (PFN_vkGetLatencyTimingsNV)    vkGetDeviceProcAddr(qvk.device, "vkGetLatencyTimingsNV");
    qvkQueueNotifyOutOfBandNV = (PFN_vkQueueNotifyOutOfBandNV) vkGetDeviceProcAddr(qvk.device, "vkQueueNotifyOutOfBandNV");

    if (!qvkSetLatencySleepModeNV || !qvkLatencySleepNV || !qvkSetLatencyMarkerNV) {
        Com_EPrintf("Reflex: VK_NV_low_latency2 is present but its entry points did not "
                    "load, low latency mode disabled\n");
        qvk.supports_low_latency = false;
        return;
    }

    if (reflex_sleep_semaphore == VK_NULL_HANDLE) {
        VkSemaphoreTypeCreateInfo type_info = {
            .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue  = 0,
        };
        VkSemaphoreCreateInfo sem_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type_info,
        };

        if (vkCreateSemaphore(qvk.device, &sem_info, NULL, &reflex_sleep_semaphore) != VK_SUCCESS) {
            Com_EPrintf("Reflex: could not create the latency sleep semaphore, "
                        "low latency mode disabled\n");
            qvk.supports_low_latency = false;
            return;
        }
        reflex_sleep_value = 0;
    }

    Com_Printf("Reflex: VK_NV_low_latency2 available%s\n",
        Reflex_PresentIdAvailable() ? " (with VK_KHR_present_id)"
                                    : " (no VK_KHR_present_id - timings may not correlate)");
}

void Reflex_Shutdown(void)
{
    if (reflex_sleep_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(qvk.device, reflex_sleep_semaphore, NULL);
        reflex_sleep_semaphore = VK_NULL_HANDLE;
    }
    reflex_swapchain = VK_NULL_HANDLE;
    reflex_mode_applied = false;
}

bool Reflex_PresentIdAvailable(void)
{
    return qvk.supports_present_id;
}

bool Reflex_Available(void)
{
    return qvk.supports_low_latency
        && qvkLatencySleepNV != NULL
        && reflex_sleep_semaphore != VK_NULL_HANDLE;
}

bool Reflex_Enabled(void)
{
    return Reflex_Available()
        && cvar_reflex != NULL
        && cvar_reflex->integer != 0;
}

void Reflex_OnSwapchainCreated(VkSwapchainKHR swapchain)
{
    reflex_swapchain = swapchain;
    reflex_mode_applied = false;
}

/* The sleep mode is per swapchain and has to be re-applied whenever the swapchain is
   recreated or the cvars change. Cheap enough to check every frame. */
static void reflex_apply_mode(void)
{
    static int last_mode = -1;
    static int last_fps_max = -1;
    static int last_effective_fps = -1;

    if (!qvk.supports_low_latency || !qvkSetLatencySleepModeNV)
        return;
    if (reflex_swapchain == VK_NULL_HANDLE)
        return;

    int mode = cvar_reflex ? cvar_reflex->integer : 0;
    int fps_max = cvar_reflex_fps_max ? cvar_reflex_fps_max->integer : 0;

    /* Frame generation wants the render rate pinned to refresh / multiplier. An explicit
       pt_reflex_fps_max still wins when it is lower - the user asked for it. */
    int paced_fps = reflex_paced_render_fps();
    int effective_fps = fps_max;
    if (paced_fps > 0 && (effective_fps == 0 || paced_fps < effective_fps))
        effective_fps = paced_fps;

    if (reflex_mode_applied && mode == last_mode && fps_max == last_fps_max
        && effective_fps == last_effective_fps)
        return;

    /* minimumIntervalUs is the frame rate cap: the minimum time between frame starts.
       0 disables it. */
    uint32_t interval_us = (effective_fps > 0) ? (uint32_t)(1000000.0 / (double)effective_fps) : 0;

    VkLatencySleepModeInfoNV info = {
        .sType             = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
        .lowLatencyMode    = (mode != 0) ? VK_TRUE : VK_FALSE,
        .lowLatencyBoost   = (mode >= 2) ? VK_TRUE : VK_FALSE,
        .minimumIntervalUs = interval_us,
    };

    FGPresent_SwapchainLock();
    VkResult res = qvkSetLatencySleepModeNV(qvk.device, reflex_swapchain, &info);
    FGPresent_SwapchainUnlock();
    if (res != VK_SUCCESS) {
        Com_WPrintf("Reflex: vkSetLatencySleepModeNV failed (%d)\n", res);
        return;
    }

    Com_Printf("Reflex: %s%s%s\n",
        mode == 0 ? "off" : "on",
        (mode >= 2) ? " + boost" : "",
        interval_us
            ? va(", render capped at %d fps%s", effective_fps,
                 (paced_fps > 0 && effective_fps == paced_fps)
                     ? va(" (paced to %d Hz display)", Reflex_DisplayRefreshHz()) : "")
            : "");

    last_mode = mode;
    last_fps_max = fps_max;
    last_effective_fps = effective_fps;
    reflex_mode_applied = true;
}

static void reflex_report_timings(void)
{
    if (!cvar_reflex_stats || !cvar_reflex_stats->integer || !qvkGetLatencyTimingsNV)
        return;
    if (reflex_swapchain == VK_NULL_HANDLE)
        return;

    static uint64_t last_report_us = 0;
    uint64_t now = Sys_Microseconds();
    if (now - last_report_us < 1000000)
        return;
    last_report_us = now;

    /* Query the count first: the driver reports however many frames it has retained. */
    VkGetLatencyMarkerInfoNV info = {
        .sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV,
        .timingCount = 0,
        .pTimings = NULL,
    };
    FGPresent_SwapchainLock();
    qvkGetLatencyTimingsNV(qvk.device, reflex_swapchain, &info);
    FGPresent_SwapchainUnlock();

    if (info.timingCount == 0)
        return;

    uint32_t count = info.timingCount;
    if (count > 64)
        count = 64;

    VkLatencyTimingsFrameReportNV reports[64];
    for (uint32_t i = 0; i < count; i++) {
        memset(&reports[i], 0, sizeof(reports[i]));
        reports[i].sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
    }

    info.timingCount = count;
    info.pTimings = reports;
    FGPresent_SwapchainLock();
    qvkGetLatencyTimingsNV(qvk.device, reflex_swapchain, &info);
    FGPresent_SwapchainUnlock();

    /* The most recent complete report. End-to-end latency is input sample to the point
       the frame finished being presented. */
    const VkLatencyTimingsFrameReportNV *r = &reports[count - 1];
    if (r->inputSampleTimeUs == 0 || r->presentEndTimeUs < r->inputSampleTimeUs)
        return;

    Com_Printf("Reflex: input->present %.1f ms (sim %.1f, render submit %.1f, "
               "gpu %.1f, present %.1f)\n",
        (double)(r->presentEndTimeUs - r->inputSampleTimeUs) / 1000.0,
        (double)(r->simEndTimeUs - r->simStartTimeUs) / 1000.0,
        (double)(r->renderSubmitEndTimeUs - r->renderSubmitStartTimeUs) / 1000.0,
        (double)(r->gpuRenderEndTimeUs - r->gpuRenderStartTimeUs) / 1000.0,
        (double)(r->presentEndTimeUs - r->presentStartTimeUs) / 1000.0);
}

void Reflex_SetMarkerForFrame(VkLatencyMarkerNV marker, uint64_t present_id)
{
    /* Available, not Enabled: markers keep flowing with the sleep switched off so the
       timing report still has something to measure. */
    if (!Reflex_Available() || !qvkSetLatencyMarkerNV)
        return;
    if (reflex_swapchain == VK_NULL_HANDLE || present_id == 0)
        return;

    VkSetLatencyMarkerInfoNV info = {
        .sType     = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
        .presentID = present_id,
        .marker    = marker,
    };
    FGPresent_SwapchainLock();
    qvkSetLatencyMarkerNV(qvk.device, reflex_swapchain, &info);
    FGPresent_SwapchainUnlock();
}

void Reflex_SetMarker(VkLatencyMarkerNV marker)
{
    Reflex_SetMarkerForFrame(marker, reflex_present_id);
}

uint64_t Reflex_CurrentPresentID(void)
{
    return reflex_present_id;
}

void Reflex_SleepAndBeginFrame(void)
{
    /* Apply mode changes even while disabled, so switching pt_reflex off actually tells
       the driver to stop rather than just skipping our sleep. */
    reflex_apply_mode();

    if (!Reflex_Available())
        return;
    if (reflex_swapchain == VK_NULL_HANDLE)
        return;

    reflex_report_timings();

    /* A new frame starts whether or not the sleep is on - the markers and the present id
       have to keep advancing so the driver can still time frames. */
    reflex_present_id++;

    if (!Reflex_Enabled()) {
        Reflex_SetMarker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
        Reflex_SetMarker(VK_LATENCY_MARKER_SIMULATION_START_NV);
        return;
    }

    reflex_sleep_value++;
    VkLatencySleepInfoNV sleep_info = {
        .sType           = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
        .signalSemaphore = reflex_sleep_semaphore,
        .value           = reflex_sleep_value,
    };

    /* Lock only around the call itself. The actual blocking wait is on the semaphore
       below, OUTSIDE the lock - holding it there would stall the present thread for the
       whole sleep, which is most of a frame. */
    FGPresent_SwapchainLock();
    VkResult res = qvkLatencySleepNV(qvk.device, reflex_swapchain, &sleep_info);
    FGPresent_SwapchainUnlock();

    if (res != VK_SUCCESS) {
        /* Do not spam: a failure here usually means the swapchain is being recreated. */
        reflex_sleep_value--;
        reflex_present_id--;
        return;
    }

    /* vkLatencySleepNV returns immediately and signals the semaphore when the frame
       should start, so the wait is what actually delays us. */
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &reflex_sleep_semaphore,
        .pValues        = &reflex_sleep_value,
    };
    vkWaitSemaphores(qvk.device, &wait_info, UINT64_MAX);

    /* Input is sampled by the caller immediately after this returns, so both markers
       belong here. */
    Reflex_SetMarker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
    Reflex_SetMarker(VK_LATENCY_MARKER_SIMULATION_START_NV);
}

void Reflex_NotifyOutOfBandPresent(VkQueue queue)
{
    if (!Reflex_Available() || !qvkQueueNotifyOutOfBandNV)
        return;

    VkOutOfBandQueueTypeInfoNV info = {
        .sType     = VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV,
        .queueType = VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV,
    };
    qvkQueueNotifyOutOfBandNV(queue, &info);
}
