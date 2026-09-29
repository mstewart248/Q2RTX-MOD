/*
DLSS 5 Neural Rendering (DLSSNR, NGX feature 18). Local experiment on the DLSS5 branch.

Runs on the finished, tone-mapped, HUD-less frame in VKPT_IMG_DLSS_OUTPUT - after bloom and
tone mapping, before frame generation and the HUD - so it needs DLSS (any pt_dlss mode) on.
The snippet is reached through nvngx.dll_dlssnr.dll (dlssnr_forwarder.cpp); see there for why.
*/
#pragma once

#include <vulkan/vulkan.h>
#include "shared/shared.h"

void DLSS5_InitCvars(void);
qboolean DLSS5Enabled(void);

/* pt_dlss5_drugs: the game is paused and each frame's DLSS 5 result is fed back in as the
   next frame's input. main.c keeps photo mode out of the way while this is on. */
qboolean DLSS5DrugsActive(void);

/* Records the pass into cmd. Call only when DLSSEnabled() and not in photo mode. */
void DLSS5Apply(VkCommandBuffer cmd, qboolean resetHistory);

/* Drops the feature and the output image; both are rebuilt lazily by the next DLSS5Apply.
   Call with the device already idle (rebuild_render_resources). */
void DLSS5_DestroyFeature(void);

/* Full teardown, including NGX on this device. Device must be idle. */
void DLSS5_Shutdown(void);

/* The sRGB encode/decode pass around the model (shader/dlss5_convert.comp). */
VkResult vkpt_dlss5_initialize(void);
VkResult vkpt_dlss5_destroy(void);
VkResult vkpt_dlss5_create_pipelines(void);
VkResult vkpt_dlss5_destroy_pipelines(void);
