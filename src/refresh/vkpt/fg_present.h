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

// Timed presentation for DLSS Frame Generation.
//
// WHY THIS EXISTS: raw NGX hands back the interpolated frames and stops - unlike
// Streamline's sl.dlss_g there is no present thread in the SDK. Issuing all of a
// frame's presents back to back does not work: MEASURED on 2026-08-29, six presents
// of a 6x group landed within ~300us of a 5000us frame, so the display scanned out
// whichever arrived last (always the real frame) and every generated frame was
// discarded unseen. The FPS counter read 200 while the image moved at 33.
//
// Pacing on the main thread cannot fix it either. With even spacing the sleep inside
// R_EndFrame totals N/(N+1) of a frame, so T = R + N*T/(N+1), i.e. T = R*(N+1): the
// render rate is divided by the multiplier and the presented rate collapses back to
// what it would have been with no frame generation. The sleep cannot overlap GPU work
// because the engine submits the whole frame and then presents.
//
// So presents are handed to this thread with a target time each, and the main thread
// carries straight on rendering.

#pragma once

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

// Creates the worker thread and its sync objects. Safe to call repeatedly.
void FGPresent_Init(void);

// Drains anything queued, stops the thread and releases the sync objects.
void FGPresent_Shutdown(void);

/* THE DISPLAY'S SCANOUT CLOCK.

   Pacing presents evenly is not enough on a fixed-refresh display: the panel shows one
   frame per vblank and takes whatever is current then, so a present train that is merely
   the right RATE drifts in phase. The display keeps sampling the same slot of each
   generated group, the group repeats once per rendered frame, and perceived motion falls
   back to the render rate however high the present counter reads. The presents have to
   LAND on vblanks.

   D3DKMTWaitForVerticalBlankEvent blocks on the adapter's real vblank, so unlike
   DwmGetCompositionTimingInfo it stays valid whether the swapchain is composited or holds
   exclusive fullscreen - DWM's composition clock describes nothing once the compositor is
   bypassed, which is why snapping to it cost 4 fps in fullscreen.
   VK_GOOGLE_display_timing would be the portable answer and is not exposed on
   Windows/NVIDIA. Resolved from gdi32.dll at runtime, so it costs the build nothing and
   degrades to "no phase lock" rather than failing.

   `gdi_device` is a GDI display name, e.g. the szDevice out of MONITORINFOEX. */
void FGPresent_VBlankStart(const char *gdi_device);
void FGPresent_VBlankStop(void);

/* Most recent vblank and the smoothed refresh period, both in the Sys_Microseconds
   timebase. False until the clock has seen enough consecutive vblanks to be trusted. */
bool FGPresent_VBlankInfo(uint64_t *out_last_us, double *out_period_us);


// Blocks until every queued present has been issued. MUST be called before the
// swapchain is destroyed - a queued present still refers to the old VkSwapchainKHR.
void FGPresent_Drain(void);

// Blocks until at most `max_pending` presents remain queued. Used before acquiring a
// new group to stay under Vulkan's limit on simultaneously acquired images, WITHOUT
// draining completely - a full drain would collapse the one-frame scheduling lead that
// lets the GPU finish before a group's first present is due.
void FGPresent_WaitUntilPending(unsigned int max_pending);

// Queues one present for `target_us` (a Sys_Microseconds() value). Returns false if
// the queue is full or the thread is not running, in which case the caller should
// present inline instead of dropping the frame.
// `min_gap_us` is a floor on the spacing from the previous present, applied on top of
// target_us. The absolute schedule assumes the frame interval can be predicted; in real
// gameplay it jitters, and a mispredicted schedule bunches presents back together. The
// floor makes even spacing hold regardless of how wrong the prediction is.
// `reflex_present_id` is non-zero only for the REAL frame of a group: that present is
// the one Reflex must see complete, so it carries the id and the PRESENT_START/END
// markers. The generated frames are presented out-of-band instead - they are not work
// the game simulated, and counting them would corrupt the driver's latency maths.
/* `group_id` identifies the rendered frame this present belongs to, and it is how the
   thread answers "is a generated frame about to be shown when a newer real frame is
   already waiting?". Deadlines belonging to a group two or more behind the newest are
   abandoned and those presents issued at once, so the queue catches up instead of
   walking through stale content. One group behind is left alone - that is the ordinary
   steady state and collapsing it would wreck the pacing. */
bool FGPresent_Enqueue(VkSwapchainKHR swapchain, uint32_t image_index,
                       VkSemaphore wait_semaphore, uint64_t target_us,
                       uint64_t min_gap_us, uint64_t reflex_present_id,
                       uint64_t group_id, uint64_t timeline_value);

// The result of the most recently issued present, consumed by the main thread to
// decide whether the swapchain needs recreating. Returns VK_SUCCESS when there is
// nothing new to report, and clears the stored value.
VkResult FGPresent_TakeLastResult(void);

/* Smoothed microseconds by which presents are missing their deadlines. The schedule adds
   this to its lead, which is a self-correcting loop: too short a lead shows up as lateness
   and lengthens the lead, and a long enough lead drives the lateness to zero. */
double FGPresent_MeanLatenessUs(void);

/* TIMELINE READINESS - why the presents carry no wait semaphore.

   vkQueuePresentKHR does not flip; it queues a flip that fires when the wait semaphore
   signals. Every present in a group waited on a semaphore signalled by the SAME submit, so
   they all became presentable at once and flipped back to back - PresentMon measured the
   pair 0.11 ms apart with one frame visible for a tenth of a millisecond, i.e. 30 visual
   updates per second while every counter read 60.

   Two attempts to fix that with a wait-only vkQueueSubmit both juddered: a single graphics
   queue executes submissions IN ORDER, so such a submit blocks the next frame's rendering
   behind it. A timeline semaphore is host-waitable - vkWaitSemaphores needs no queue
   operation at all - so the present thread can learn the work is done without putting
   anything in the queue's path.

   FGPresent_SignalTimeline() is called once per frame right after the frame submit and
   returns the value to hand to FGPresent_Enqueue(). */
bool     FGPresent_TimelineAvailable(void);
uint64_t FGPresent_SignalTimeline(VkQueue queue);
/* Host-side wait on that timeline value. Used by the inline fallback presents, which
   otherwise would wait on a binary semaphore the frame submit never signals. */
bool     FGPresent_WaitTimeline(uint64_t value);

// Guards BOTH the VkQueue and the VkSwapchainKHR. Vulkan requires external
// synchronisation for each, and the present thread touches both while the main thread is
// acquiring, submitting and setting Reflex markers. Every one of these is a swapchain or
// queue operation and must be wrapped:
//   vkQueueSubmit, vkQueuePresentKHR, vkAcquireNextImage[2]KHR,
//   vkLatencySleepNV, vkSetLatencyMarkerNV, vkSetLatencySleepModeNV, vkGetLatencyTimingsNV
//
// DEADLOCK WARNING: never hold this across a blocking vkAcquireNextImageKHR. The images
// it waits for are released by presents that need this same lock. Poll with a short
// timeout and drop the lock between attempts - see acquire_next_image_locked().
void FGPresent_SwapchainLock(void);
void FGPresent_SwapchainUnlock(void);
