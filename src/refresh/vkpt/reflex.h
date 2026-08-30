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

// NVIDIA Reflex, via the VK_NV_low_latency2 device extension.
//
// WHY THIS IS NOT NvLowLatencyVk.dll: `streamline/NvLowLatencyVk.dll` in this tree is
// the OLD way in, from a Streamline drop that nothing here references. Modern Reflex is
// a plain Vulkan extension, so it needs no DLL, no SDK and no interposer - it only
// became visible here because the Vulkan headers were bumped off 1.2.162, which predates
// the extension entirely.
//
// WHAT IT DOES: without it the CPU runs ahead of the GPU and queues up frames, so an
// input sampled at the top of a frame is not seen until several frames later.
// vkLatencySleepNV holds the start of the frame until the driver judges the GPU is
// ready, which cuts that queue depth. It matters most with frame generation, which
// deliberately adds a frame of lead so the interpolated frames have somewhere to sit.

#pragma once

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

// Refresh rate in Hz of the display the window is on, or 0 if it cannot be determined.
// Available regardless of whether Reflex itself is supported.
int Reflex_DisplayRefreshHz(void);

// Loads the entry points. Call once after the device exists.
void Reflex_Init(void);

// Releases the timeline semaphore. Call before the device goes away.
void Reflex_Shutdown(void);

// True when the extension and its entry points are usable, whatever pt_reflex says.
// Markers, present ids and timing queries all stay live when the sleep is switched
// off, so latency can be MEASURED with Reflex off - otherwise there is no baseline to
// compare against and no way to show the feature does anything.
bool Reflex_Available(void);

// True when the extension is present AND the user has the sleep switched on.
bool Reflex_Enabled(void);

// Applies the current cvar settings to `swapchain`. Call after every swapchain
// creation - the mode is a property of the swapchain, not the device.
void Reflex_OnSwapchainCreated(VkSwapchainKHR swapchain);

// Blocks until the driver says this frame should start, then opens a new frame.
// Call at the TOP of the client frame, before input is sampled - that placement is
// the entire point, since the sleep is what stops the CPU running ahead.
void Reflex_SleepAndBeginFrame(void);

// Marks a point in the current frame. Safe to call when Reflex is off (does nothing).
void Reflex_SetMarker(VkLatencyMarkerNV marker);

// Same, but for an explicit frame. The present thread lags the main thread, so by the
// time it presents a frame the "current" id has already moved on - it must name the
// frame it is actually presenting.
void Reflex_SetMarkerForFrame(VkLatencyMarkerNV marker, uint64_t present_id);

// The presentID for the frame being built, to tag its present with.
uint64_t Reflex_CurrentPresentID(void);

// Tells the driver that `queue` is presenting outside the normal render loop, which is
// what the frame generation present thread does. Without this the driver reads those
// presents as application render submissions and its pacing maths goes wrong.
void Reflex_NotifyOutOfBandPresent(VkQueue queue);

// True when VkPresentIdKHR may be chained onto a present (the extension and its feature
// are both enabled). The driver needs it to tie a present back to its markers.
bool Reflex_PresentIdAvailable(void);
