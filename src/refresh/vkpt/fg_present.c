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

// Timed presentation for DLSS Frame Generation - see fg_present.h for why.

#include <SDL.h>
#include <SDL_thread.h>

#ifdef _WIN32
/* NOMINMAX matters here for the same reason as in main.c: shared.h has min/max. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "shared/shared.h"
#include "common/common.h"
#include "common/cvar.h"
/* Sys_Microseconds() lives here. WITHOUT this include C implicitly declares it as
   returning int, which truncates the 64-bit microsecond clock to garbage - every
   deadline then compares as already passed and the thread never waits at all.
   It shows up only as a C4013 warning, and this build emits ~23 of those. */
#include "system/system.h"
#include "vkpt.h"
#include "fg_present.h"
#include "reflex.h"

// One rendered frame queues at most (multiplier) presents, so this holds several
// frames' worth. Overflow is handled by the caller presenting inline, never by
// dropping a frame on the floor.
#define FG_PRESENT_QUEUE_SIZE 32

typedef struct {
    VkSwapchainKHR swapchain;
    uint32_t       image_index;
    VkSemaphore    wait_semaphore;
    uint64_t       target_us;
    uint64_t       min_gap_us;
    uint64_t       queued_at_us;
    uint64_t       reflex_present_id;   // 0 = generated frame, present out of band
    uint64_t       group_id;            // the rendered frame this belongs to
    uint64_t       timeline_value;      // frame work complete at this value
} fg_present_item_t;

static fg_present_item_t fg_queue[FG_PRESENT_QUEUE_SIZE];
static int          fg_queue_head = 0;
static int          fg_queue_count = 0;
static uint64_t     fg_newest_group = 0;

static SDL_Thread  *fg_thread = NULL;
static SDL_mutex   *fg_queue_mutex = NULL;   // guards the ring buffer
static SDL_cond    *fg_queue_added = NULL;   // signalled when an item is queued
static SDL_cond    *fg_queue_drained = NULL; // signalled when an item is issued
static SDL_mutex   *fg_vkqueue_mutex = NULL; // guards the VkQueue AND the VkSwapchainKHR
static volatile bool fg_quit = false;

static VkResult     fg_last_result = VK_SUCCESS;

/* Smoothed lateness of the presents against their deadlines. The render thread feeds this
   back into the scheduling lead: late presents mean the lead is too short. It is NEGATIVE
   feedback - more lead produces less lateness - which is what makes it safe to close the
   loop here, unlike deriving the lead from a measured frame interval. */
static volatile double fg_late_ema_us = 0.0;

/* pt_dlss_fg_stats 1 reports the gaps between presents as they are ACTUALLY issued -
   the only number that says whether pacing is working. Bunched gaps mean the generated
   frames are being overwritten before the display ever scans them out. */
static cvar_t      *cvar_fg_stats = NULL;

static void fg_report_spacing(uint64_t issued_us, uint64_t target_us, uint64_t queued_at_us)
{
    if (!cvar_fg_stats)
        cvar_fg_stats = Cvar_Get("pt_dlss_fg_stats", "0", 0);

    if (!cvar_fg_stats->integer)
        return;

    static uint64_t prev_us = 0;
    static uint64_t last_report_us = 0;
    static unsigned int n = 0;
    static uint64_t gaps[16];

    if (prev_us != 0 && n < 16)
        gaps[n++] = issued_us - prev_us;
    prev_us = issued_us;

    if (n >= 12 && issued_us - last_report_us > 1000000) {
        last_report_us = issued_us;
        char line[256];
        int off = Q_snprintf(line, sizeof(line), "DLSS-G present gaps us:");
        for (unsigned int i = 0; i < n; i++)
            off += Q_snprintf(line + off, sizeof(line) - off, " %llu",
                              (unsigned long long)gaps[i]);
        Com_Printf("%s\n", line);
        /* How late this present was against its deadline, and how long the item sat
           in the queue. A large "late" means the deadlines were already in the past
           when the thread reached them - the schedule is wrong, not the waiting. */
        Com_Printf("DLSS-G   last present: late %lld us, queued %llu us before issue\n",
                   (long long)((int64_t)issued_us - (int64_t)target_us),
                   (unsigned long long)(issued_us - queued_at_us));
        n = 0;
    }
    else if (n >= 16) {
        n = 0;
    }
}

/* The swapchain/queue mutex is a process-wide singleton and is deliberately NEVER
   destroyed. It used to be created in FGPresent_Init() and destroyed in
   FGPresent_Shutdown(), which left a window - around vid_restart and renderer teardown -
   where it was NULL while queue work was still in flight. Every lock taken in that window
   silently did nothing, which is exactly the shape of the intermittent "VkQueue is
   simultaneously used in current thread A and thread B" reports that survived locking
   every individual call site.

   Leaking one SDL mutex for the life of the process is the right trade. */
static SDL_mutex *fg_get_vkqueue_mutex(void)
{
    if (!fg_vkqueue_mutex) {
        SDL_mutex *m = SDL_CreateMutex();
        if (!m)
            return NULL;
        if (fg_vkqueue_mutex)
            SDL_DestroyMutex(m);   /* benign first-call race: free the spare */
        else
            fg_vkqueue_mutex = m;
    }
    return fg_vkqueue_mutex;
}

void FGPresent_SwapchainLock(void)
{
    SDL_mutex *m = fg_get_vkqueue_mutex();
    if (m)
        SDL_LockMutex(m);
}

void FGPresent_SwapchainUnlock(void)
{
    if (fg_vkqueue_mutex)
        SDL_UnlockMutex(fg_vkqueue_mutex);
}

/* Sleep until `target_us`. SDL_Delay only has millisecond resolution and habitually
   overshoots, and the slots here are a few hundred microseconds to a few milliseconds
   apart - overshooting one slot pushes every later frame of the group late. So sleep
   only while there is more than a millisecond of slack and spin out the remainder. The
   spin is bounded by that millisecond and this thread does nothing else. */
static void fg_wait_until(uint64_t target_us)
{
    for (;;) {
        uint64_t now = Sys_Microseconds();
        if (now >= target_us)
            return;

        uint64_t remaining = target_us - now;

        if (remaining > 2000)
            SDL_Delay((Uint32)((remaining - 1000) / 1000));
        else
            SDL_Delay(0);   // yield; the caller re-checks the clock
    }
}


/* MAKE THE FRAME PRESENTABLE BEFORE THE DEADLINE, NOT AT IT.

   vkQueuePresentKHR does not flip. It queues a flip that fires when the wait semaphore
   signals - so the SIGNAL time controls when the image reaches the screen, not the time we
   call present. Every present in a group waits on a semaphore signalled by the SAME
   vkQueueSubmit (one submit records all the blits), so all of them became presentable at
   the same instant and the presentation engine flipped them back to back.

   PresentMon measured it from outside the process at 2x:

     MsBetweenPresents:  16.63 16.66 16.67 16.66   <- our calls, perfectly paced
     MsBetweenDisplay:   33.51  0.11 33.25  0.11   <- what the display actually got
     MsUntilDisplayed:   59.54 42.99 59.57 43.02   <- differ by exactly one interval

   Both frames of each pair land at the same wall-clock moment; one is on screen for 0.11 ms
   and is never seen. 30 visual updates per second while every counter reads 60. That is why
   pacing the present CALLS - the scanout clock, the phase lock, the lead, the floor - never
   changed anything on screen, and why Matt kept correctly saying it was not working.

   Fix: consume the semaphore on this thread first, with a wait-only submit and a fence, so
   the image is ALREADY presentable. Then sleep to the deadline and present with no wait
   semaphore, which flips at the call. Readiness is established before the deadline rather
   than at it, so the flip lands on time instead of late.

   THIS IMPLEMENTATION IS WRONG AND DEFAULTS OFF. A single graphics queue executes
   submissions IN ORDER, so a wait-only submit blocks everything behind it - including the
   NEXT frame's rendering - until the signal arrives. It does not remove a stall, it moves
   one into the render path, and it juddered badly at both ALL_COMMANDS and TOP_OF_PIPE and
   at both once-per-present and once-per-group.

   The diagnosis is still right; only this way of acting on it is wrong. The correct fix is
   a TIMELINE semaphore signalled by the frame submit, host-waited on the present thread
   with vkWaitSemaphores - no queue submission at all, so nothing can stall behind it.
   timelineSemaphore is already enabled in this device (Reflex needs it). The binary
   per-image present semaphores then come off the FG path entirely and the presents carry
   no wait semaphore. */
static VkFence fg_ready_fence = VK_NULL_HANDLE;

static bool fg_make_ready(VkSemaphore sem)
{
    if (fg_ready_fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(qvk.device, &fence_info, NULL, &fg_ready_fence) != VK_SUCCESS) {
            fg_ready_fence = VK_NULL_HANDLE;
            return false;
        }
    }

    /* TOP_OF_PIPE: this submit executes nothing, it only observes the signal.
       ALL_COMMANDS made the graphics queue serialise against the main thread's work
       and produced heavy judder. */
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &sem,
        .pWaitDstStageMask  = &stage,
    };

    FGPresent_SwapchainLock();
    VkResult res = vkQueueSubmit(qvk.queue_graphics, 1, &submit, fg_ready_fence);
    FGPresent_SwapchainUnlock();

    if (res != VK_SUCCESS)
        return false;

    vkWaitForFences(qvk.device, 1, &fg_ready_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(qvk.device, 1, &fg_ready_fence);
    return true;
}


/* A/B switch for the dedicated present queue (default on). */
static bool fg_use_present_queue(void)
{
    static cvar_t *cv_queue = NULL;
    if (!cv_queue) cv_queue = Cvar_Get("pt_dlss_fg_queue", "1", 0);
    return cv_queue->integer != 0 && qvk.queue_present_dedicated;
}


/* ---- timeline readiness: see fg_present.h ---- */
static VkSemaphore fg_timeline = VK_NULL_HANDLE;
static uint64_t    fg_timeline_next = 0;
static bool        fg_timeline_failed = false;

bool FGPresent_TimelineAvailable(void)
{
    /* DEFAULT ON, and it only works together with the dedicated present queue.

       There were TWO gates holding a group's flips together, and removing either one on
       its own changed nothing:

         1. THE QUEUE. Presents used to be issued on qvk.queue_graphics, and a queue
            processes its submissions in order, so a present issued after the next frame's
            render submit could not flip until that render finished on the GPU. Fixed by
            presenting on qvk.queue_present.
         2. THE SEMAPHORE. Every present in a group waited on a binary semaphore signalled
            by the SAME vkQueueSubmit - one submit records all the blits - so all of them
            became presentable at the same instant. Fixed here: wait for that submit ONCE
            per group on the HOST (a timeline semaphore, no queue operation, so nothing
            can stall behind it), then present each frame at its deadline carrying no wait
            semaphore at all, which flips at the call.

       Turning this on alone was tested and left MsBetweenDisplayChange at 33.5/0.11
       because gate 1 still held. Do not read that result as evidence against it. */
    static cvar_t *cv_timeline = NULL;
    if (!cv_timeline) cv_timeline = Cvar_Get("pt_dlss_fg_timeline", "1", 0);
    if (!cv_timeline->integer)
        return false;

    if (fg_timeline != VK_NULL_HANDLE)
        return true;
    if (fg_timeline_failed)
        return false;

    VkSemaphoreTypeCreateInfo type_info = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0,
    };
    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_info,
    };

    if (vkCreateSemaphore(qvk.device, &sem_info, NULL, &fg_timeline) != VK_SUCCESS) {
        fg_timeline = VK_NULL_HANDLE;
        fg_timeline_failed = true;
        Com_WPrintf("DLSS-G: no timeline semaphore, presents fall back to binary waits "
                    "and will flip in bursts\n");
        return false;
    }

    Com_Printf("DLSS-G: timeline readiness active - presents flip when issued\n");
    return true;
}

uint64_t FGPresent_SignalTimeline(VkQueue queue)
{
    if (!FGPresent_TimelineAvailable())
        return 0;

    uint64_t value = ++fg_timeline_next;

    /* Signals only, waits on nothing: it runs after the frame submit already in the queue
       and cannot block anything behind it. */
    VkTimelineSemaphoreSubmitInfo timeline_info = {
        .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues    = &value,
    };
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = &timeline_info,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &fg_timeline,
    };

    FGPresent_SwapchainLock();
    VkResult res = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    FGPresent_SwapchainUnlock();

    if (res != VK_SUCCESS)
        return 0;
    return value;
}

/* Host wait - no queue operation, so nothing queues behind it. */
static bool fg_timeline_wait(uint64_t value)
{
    if (value == 0 || fg_timeline == VK_NULL_HANDLE)
        return false;

    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &fg_timeline,
        .pValues        = &value,
    };
    return vkWaitSemaphores(qvk.device, &wait_info, UINT64_MAX) == VK_SUCCESS;
}

/* Same wait, for the INLINE fallback presents on the main thread. When the timeline path
   is active the per-image binary present semaphores are never signalled (see main.c), so
   a fallback present that waited on one would hang the presentation engine forever. It
   must establish readiness the same way the present thread does and then present with no
   wait semaphore. */
bool FGPresent_WaitTimeline(uint64_t value)
{
    return fg_timeline_wait(value);
}

static int SDLCALL fg_present_thread(void *unused)
{
    (void)unused;

    SDL_LockMutex(fg_queue_mutex);

    while (!fg_quit) {
        while (fg_queue_count == 0 && !fg_quit)
            SDL_CondWait(fg_queue_added, fg_queue_mutex);

        if (fg_quit)
            break;

        fg_present_item_t item = fg_queue[fg_queue_head];

        /* Two or more groups behind means the deadlines in front of us describe moments
           that have already passed on screen, and a newer real frame is waiting behind
           them. Honouring that schedule would walk the display through stale generated
           frames; issue them at once and let the newest content land instead. */
        bool stale = (fg_newest_group > item.group_id + 1);

        /* Release the queue while waiting and presenting so the main thread can keep
           queueing the rest of the group. The item is not popped until it has actually
           been issued, which is what makes FGPresent_Drain() meaningful. */
        SDL_UnlockMutex(fg_queue_mutex);

        /* Honour the absolute deadline, but never issue closer than min_gap_us to the
           previous present. The deadline positions the group correctly relative to the
           GPU; the floor is what actually guarantees even spacing when the predicted
           frame interval turns out to be wrong. */
        static uint64_t last_issue_us = 0;
        uint64_t effective_target = stale ? 0 : item.target_us;
        if (!stale && last_issue_us != 0 && item.min_gap_us > 0) {
            uint64_t earliest = last_issue_us + item.min_gap_us;
            if (earliest > effective_target)
                effective_target = earliest;
        }

        /* Establish presentability BEFORE sleeping to the deadline, so the flip happens
           at the deadline rather than whenever the GPU happens to finish. */
        /* Wait on the HOST for this frame's GPU work, once per group, then sleep to the
           deadline and present with no wait semaphore - so the flip happens at the call.
           See fg_present.h for the measurement that made this necessary. */
        static uint64_t fg_ready_group = (uint64_t)-1;
        bool image_ready = false;
        if (item.timeline_value != 0) {
            if (item.group_id != fg_ready_group) {
                if (fg_timeline_wait(item.timeline_value))
                    fg_ready_group = item.group_id;
            }
            image_ready = (item.group_id == fg_ready_group);
        }

        fg_wait_until(effective_target);

        VkPresentInfoKHR present_info = {
            .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            /* No wait semaphore once the image is already presentable: the flip then
               fires at this call instead of at some future signal. */
            .waitSemaphoreCount = image_ready ? 0 : 1,
            .pWaitSemaphores    = image_ready ? NULL : &item.wait_semaphore,
            .swapchainCount     = 1,
            .pSwapchains        = &item.swapchain,
            .pImageIndices      = &item.image_index,
            .pResults           = NULL,
        };

        /* The real frame carries the Reflex id, so the driver can see the frame it slept
           for actually reach the screen. The generated frames are presented out of band:
           they are not work the game simulated, and counting them as render submissions
           corrupts the driver's latency maths. */
        VkPresentIdKHR present_id_info = {
            .sType          = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .swapchainCount = 1,
            .pPresentIds    = &item.reflex_present_id,
        };
        if (item.reflex_present_id && Reflex_PresentIdAvailable())
            present_info.pNext = &present_id_info;

        FGPresent_SwapchainLock();

        if (item.reflex_present_id)
            Reflex_SetMarkerForFrame(VK_LATENCY_MARKER_PRESENT_START_NV, item.reflex_present_id);
        else
            Reflex_NotifyOutOfBandPresent(fg_use_present_queue() ? qvk.queue_present
                                                                 : qvk.queue_graphics);

        /* qvk.queue_present, NOT queue_graphics. A present is a queue operation and one
           queue processes its submissions in order, so a present issued here after the
           main thread has already submitted the NEXT frame's rendering cannot flip until
           that rendering completes on the GPU - which is why a whole group's flips landed
           together at the render rate however evenly these calls were spaced. See the
           comment on qvk.queue_present in vkpt.h.

           pt_dlss_fg_queue 0 puts presents back on the graphics queue, so the fix can be
           A/B'd against the old behaviour in one build. Judge it ONLY on PresentMon's
           MsBetweenDisplayChange - every in-process measurement is blind to this. */
        VkQueue present_queue = fg_use_present_queue() ? qvk.queue_present
                                                       : qvk.queue_graphics;
        VkResult res = vkQueuePresentKHR(present_queue, &present_info);

        if (item.reflex_present_id)
            Reflex_SetMarkerForFrame(VK_LATENCY_MARKER_PRESENT_END_NV, item.reflex_present_id);

        FGPresent_SwapchainUnlock();

        last_issue_us = Sys_Microseconds();

        /* SELF-DIAGNOSIS. A deadline already in the past when the thread reaches it means
           the schedule is steering nothing - the GPU is. The group then unblocks together
           on the render semaphore, the display latches one frame per RENDERED frame, and
           the result reads as the base rate with no tearing however many presents per
           second the counter reports. It is invisible without a number and it has now
           cost several rounds, so say it out loud rather than waiting for someone to
           switch on pt_dlss_fg_stats. */
        {
            static int    late_n = 0;
            static double late_sum = 0.0;
            static uint64_t late_warned_us = 0;

            int64_t late = (int64_t)last_issue_us - (int64_t)item.target_us;
            if (late < 0)
                late = 0;
            fg_late_ema_us = fg_late_ema_us * 0.95 + (double)late * 0.05;
            late_sum += (double)late;

            if (++late_n >= 240) {
                double mean = late_sum / (double)late_n;
                if (mean > 8000.0 && last_issue_us - late_warned_us > 30000000ULL) {
                    late_warned_us = last_issue_us;
                    Com_WPrintf("DLSS-G: presents are landing %.1f ms late on average. The "
                                "schedule is expiring before the GPU finishes, so the group "
                                "unblocks all at once and the display shows one frame per "
                                "rendered frame. Raise pt_dlss_fg_lead.\n", mean / 1000.0);
                }
                late_n = 0;
                late_sum = 0.0;
            }
        }

        fg_report_spacing(Sys_Microseconds(), item.target_us, item.queued_at_us);

        SDL_LockMutex(fg_queue_mutex);

        /* Report only failures. Reporting VK_SUCCESS would clobber a pending
           OUT_OF_DATE that the main thread has not collected yet. */
        if (res != VK_SUCCESS)
            fg_last_result = res;

        fg_queue_head = (fg_queue_head + 1) % FG_PRESENT_QUEUE_SIZE;
        fg_queue_count--;
        SDL_CondBroadcast(fg_queue_drained);
    }

    SDL_UnlockMutex(fg_queue_mutex);
    return 0;
}

void FGPresent_Init(void)
{
    if (fg_thread)
        return;

    fg_queue_mutex   = SDL_CreateMutex();
    fg_queue_added   = SDL_CreateCond();
    fg_queue_drained = SDL_CreateCond();
    (void)fg_get_vkqueue_mutex();

    if (!fg_queue_mutex || !fg_queue_added || !fg_queue_drained || !fg_vkqueue_mutex) {
        Com_EPrintf("DLSS-G: could not create present thread sync objects, "
                    "frame pacing disabled\n");
        FGPresent_Shutdown();
        return;
    }

    fg_quit = false;
    fg_queue_head = 0;
    fg_queue_count = 0;
    fg_last_result = VK_SUCCESS;

    fg_thread = SDL_CreateThread(fg_present_thread, "dlssg-present", NULL);
    if (!fg_thread) {
        Com_EPrintf("DLSS-G: could not create present thread (%s), "
                    "frame pacing disabled\n", SDL_GetError());
        FGPresent_Shutdown();
        return;
    }

    Com_Printf("DLSS-G: present thread started\n");
}

void FGPresent_Drain(void)
{
    if (!fg_queue_mutex)
        return;

    SDL_LockMutex(fg_queue_mutex);
    while (fg_queue_count > 0)
        SDL_CondWait(fg_queue_drained, fg_queue_mutex);
    SDL_UnlockMutex(fg_queue_mutex);
}

void FGPresent_WaitUntilPending(unsigned int max_pending)
{
    if (!fg_queue_mutex)
        return;

    SDL_LockMutex(fg_queue_mutex);
    while ((unsigned int)fg_queue_count > max_pending)
        SDL_CondWait(fg_queue_drained, fg_queue_mutex);
    SDL_UnlockMutex(fg_queue_mutex);
}

void FGPresent_Shutdown(void)
{
    FGPresent_VBlankStop();

    if (fg_thread) {
        FGPresent_Drain();

        SDL_LockMutex(fg_queue_mutex);
        fg_quit = true;
        SDL_CondBroadcast(fg_queue_added);
        SDL_UnlockMutex(fg_queue_mutex);

        SDL_WaitThread(fg_thread, NULL);
        fg_thread = NULL;
    }

    if (fg_queue_added)   { SDL_DestroyCond(fg_queue_added);    fg_queue_added = NULL; }
    if (fg_queue_drained) { SDL_DestroyCond(fg_queue_drained);  fg_queue_drained = NULL; }
    if (fg_queue_mutex)   { SDL_DestroyMutex(fg_queue_mutex);   fg_queue_mutex = NULL; }
    /* fg_vkqueue_mutex is NOT destroyed - see fg_get_vkqueue_mutex(). */

    /* THE DEVICE-LIFETIME OBJECTS MUST GO TOO, AND THE STATICS MUST BE RESET.

       fg_timeline and fg_ready_fence belong to qvk.device. A DLSS quality-mode change
       calls viewsize_changed(), which does Cvar_SetByVar(vid_rtx, "0") then "1" - a full
       renderer restart that destroys and recreates the device. Leaving these handles
       populated meant FGPresent_TimelineAvailable() saw a non-null fg_timeline on the way
       back up, returned early WITHOUT recreating it, and the present thread then signalled
       and host-waited a semaphore belonging to a destroyed device. The readiness gate that
       decides when a frame is presentable was therefore meaningless, and presents fired
       against images that were not ready - which reads on screen as everything flashing
       and the wrong frames being shown.

       This only became reachable when pt_dlss_fg_timeline started defaulting to 1; with
       the old default of 0 the semaphore was never created, so the stale handle never
       existed. Resetting the flags matters as much as destroying the objects:
       fg_timeline_failed would otherwise latch a failure from a dead device forever, and
       fg_timeline_next has to restart at 0 alongside a freshly created semaphore. */
    if (qvk.device != VK_NULL_HANDLE) {
        /* The frame submit that signals fg_timeline may still be in flight, and
           destroying a semaphore with a pending operation is a use-after-free. The
           present thread is already joined above, so nothing else is touching a
           queue and this wait cannot race the way vkDeviceWaitIdle otherwise can. */
        vkDeviceWaitIdle(qvk.device);

        if (fg_timeline != VK_NULL_HANDLE)
            vkDestroySemaphore(qvk.device, fg_timeline, NULL);
        if (fg_ready_fence != VK_NULL_HANDLE)
            vkDestroyFence(qvk.device, fg_ready_fence, NULL);
    }
    fg_timeline = VK_NULL_HANDLE;
    fg_timeline_next = 0;
    fg_timeline_failed = false;
    fg_ready_fence = VK_NULL_HANDLE;
}

bool FGPresent_Enqueue(VkSwapchainKHR swapchain, uint32_t image_index,
                       VkSemaphore wait_semaphore, uint64_t target_us,
                       uint64_t min_gap_us, uint64_t reflex_present_id,
                       uint64_t group_id, uint64_t timeline_value)
{
    if (!fg_thread || !fg_queue_mutex)
        return false;

    SDL_LockMutex(fg_queue_mutex);

    if (fg_queue_count >= FG_PRESENT_QUEUE_SIZE) {
        SDL_UnlockMutex(fg_queue_mutex);
        return false;
    }

    int tail = (fg_queue_head + fg_queue_count) % FG_PRESENT_QUEUE_SIZE;
    fg_queue[tail].swapchain      = swapchain;
    fg_queue[tail].image_index    = image_index;
    fg_queue[tail].wait_semaphore = wait_semaphore;
    fg_queue[tail].target_us      = target_us;
    fg_queue[tail].min_gap_us     = min_gap_us;
    fg_queue[tail].reflex_present_id = reflex_present_id;
    fg_queue[tail].group_id       = group_id;
    fg_queue[tail].timeline_value = timeline_value;
    fg_queue[tail].queued_at_us   = Sys_Microseconds();
    if (group_id > fg_newest_group)
        fg_newest_group = group_id;
    fg_queue_count++;

    SDL_CondSignal(fg_queue_added);
    SDL_UnlockMutex(fg_queue_mutex);
    return true;
}

VkResult FGPresent_TakeLastResult(void)
{
    if (!fg_queue_mutex)
        return VK_SUCCESS;

    SDL_LockMutex(fg_queue_mutex);
    VkResult res = fg_last_result;
    fg_last_result = VK_SUCCESS;
    SDL_UnlockMutex(fg_queue_mutex);
    return res;
}


/* ==========================================================================
   The display's scanout clock - see fg_present.h for why this exists.
   ========================================================================== */

static SDL_Thread   *vb_thread = NULL;
static volatile bool vb_quit = false;

/* Written by the vblank thread, read by the render thread. Both are naturally aligned and
   the reader tolerates a torn pair - it re-reads next frame, and a one-frame-stale anchor
   is worth far less than a lock on the render thread's critical path. */
static volatile uint64_t vb_last_us   = 0;
static volatile double   vb_period_us = 0.0;
static volatile int      vb_samples   = 0;

#ifdef _WIN32

/* Hand-declared rather than pulling in d3dkmthk.h, which drags a great deal of kernel
   plumbing into a file that needs three fields. These layouts are stable and documented. */
typedef struct {
    WCHAR  DeviceName[32];
    UINT32 hAdapter;
    LUID   AdapterLuid;
    UINT32 VidPnSourceId;
} FG_OpenAdapterFromGdiDisplayName;

typedef struct {
    UINT32 hAdapter;
    UINT32 hDevice;
    UINT32 VidPnSourceId;
} FG_WaitForVerticalBlankEvent;

typedef LONG (APIENTRY *PFN_FG_OpenAdapter)(FG_OpenAdapterFromGdiDisplayName *);
typedef LONG (APIENTRY *PFN_FG_WaitVBlank)(const FG_WaitForVerticalBlankEvent *);

static char vb_device_name[64];

static int SDLCALL fg_vblank_thread(void *unused)
{
    (void)unused;

    HMODULE gdi = LoadLibraryA("gdi32.dll");
    PFN_FG_OpenAdapter openAdapter = gdi ? (PFN_FG_OpenAdapter)
        GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName") : NULL;
    PFN_FG_WaitVBlank waitVBlank = gdi ? (PFN_FG_WaitVBlank)
        GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent") : NULL;

    if (!openAdapter || !waitVBlank) {
        Com_WPrintf("DLSS-G: no D3DKMT vblank clock, frame generation cannot phase-lock\n");
        return 0;
    }

    FG_OpenAdapterFromGdiDisplayName open;
    memset(&open, 0, sizeof(open));
    MultiByteToWideChar(CP_ACP, 0, vb_device_name, -1, open.DeviceName, 32);

    if (openAdapter(&open) != 0) {
        Com_WPrintf("DLSS-G: could not open display adapter for the vblank clock\n");
        return 0;
    }

    FG_WaitForVerticalBlankEvent wait;
    memset(&wait, 0, sizeof(wait));
    wait.hAdapter = open.hAdapter;
    wait.hDevice = 0;
    wait.VidPnSourceId = open.VidPnSourceId;

    Com_Printf("DLSS-G: vblank clock running on %s\n", vb_device_name);

    while (!vb_quit) {
        if (waitVBlank(&wait) != 0) {
            /* The adapter can go away across a mode switch. Back off rather than spin;
               the next successful wait re-establishes the clock on its own. */
            vb_samples = 0;
            SDL_Delay(4);
            continue;
        }

        uint64_t now = Sys_Microseconds();
        uint64_t prev = vb_last_us;
        vb_last_us = now;

        if (prev == 0)
            continue;

        double dt = (double)(now - prev);

        /* A missed wakeup shows up as an exact multiple of the period. Fold those back in
           rather than discarding them, or a loaded machine never accumulates samples. */
        if (vb_period_us > 0.0) {
            double n = dt / vb_period_us;
            if (n > 1.5 && n < 8.5)
                dt /= (double)(int)(n + 0.5);
        }

        if (dt < 2000.0 || dt > 60000.0) {
            vb_samples = 0;
            continue;
        }

        vb_period_us = (vb_period_us > 0.0) ? vb_period_us * 0.90 + dt * 0.10 : dt;
        if (vb_samples < 64)
            vb_samples++;
    }

    return 0;
}

void FGPresent_VBlankStart(const char *gdi_device)
{
    if (vb_thread || !gdi_device || !*gdi_device)
        return;

    Q_strlcpy(vb_device_name, gdi_device, sizeof(vb_device_name));

    vb_quit = false;
    vb_last_us = 0;
    vb_period_us = 0.0;
    vb_samples = 0;

    vb_thread = SDL_CreateThread(fg_vblank_thread, "dlssg-vblank", NULL);
    if (!vb_thread)
        Com_WPrintf("DLSS-G: could not start the vblank clock thread (%s)\n", SDL_GetError());
}

#else

void FGPresent_VBlankStart(const char *gdi_device) { (void)gdi_device; }

#endif  /* _WIN32 */

void FGPresent_VBlankStop(void)
{
    if (!vb_thread)
        return;

    /* The thread is parked inside a vblank wait, so it wakes within one refresh. */
    vb_quit = true;
    SDL_WaitThread(vb_thread, NULL);
    vb_thread = NULL;
    vb_samples = 0;
}

bool FGPresent_VBlankInfo(uint64_t *out_last_us, double *out_period_us)
{
    /* Eight consecutive good intervals before this is allowed to steer anything. */
    if (vb_samples < 8 || vb_period_us <= 0.0)
        return false;

    if (out_last_us)   *out_last_us = vb_last_us;
    if (out_period_us) *out_period_us = vb_period_us;
    return true;
}

double FGPresent_MeanLatenessUs(void)
{
    return fg_late_ema_us;
}
