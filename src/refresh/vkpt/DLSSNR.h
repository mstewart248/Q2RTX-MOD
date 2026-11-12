/*
DLSS 5 Neural Rendering (DLSSNR, NGX feature 18), on both renderers; off and hidden unless
nvngx_dlssnr.dll sits next to q2rtx.exe.

Runs on the finished, tone-mapped, HUD-less frame in VKPT_IMG_DLSS_OUTPUT - after bloom and
tone mapping, before frame generation and the HUD - so it needs DLSS (any pt_dlss mode) on.
It goes through the driver's NGX core on the instance DLSS.c set up (feature id 18), like
RTX Remix; the core loads nvngx_dlssnr.dll from next to q2rtx.exe itself. See DLSSNR.c.
*/
#pragma once

#include <vulkan/vulkan.h>
#include <nvsdk_ngx.h>
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

/* Releases the feature and the parameter block - everything held from NGX. DLSSDeconstructor
   calls it before it shuts NGX down, since both live on DLSS.c's NGX instance. Idempotent;
   device must be idle. */
void DLSS5_ReleaseNGX(void);

/* Full teardown: DLSS5_ReleaseNGX, the feedback image, the console command. Device must
   be idle. */
void DLSS5_Shutdown(void);

/* dlss5_convert.comp's push constants (both renderers record the pass). */
typedef struct {
    int32_t extent[2];
    int32_t mode;       /* 0 encode, 1 resolve */
    int32_t split;
    float   strength;
    float   colour;
    int32_t view;
    int32_t hdr;        /* scRGB swapchain: map to/from the SDR-equivalent frame */
} dlss5_convert_push_t;

/* THE API HALF, per renderer. NULL (the default, and R_Init_RTX's) is Vulkan's NGX calls;
   the D3D12 renderer gives its own (d3d12/frontend.c): the capability block of the NGX
   instance on the running device (NULL: NGX is not up), a parameter block of DLSS 5's own
   from it, the feature's creation into `cmd` (feature id 18) and its release, and a wait
   for the idle GPU. Without `feedback` there is no pt_dlss5_drugs (its loop is copies of
   Vulkan images). */
typedef struct {
    NVSDK_NGX_Parameter*    (*instance_params)(void);
    NVSDK_NGX_Result        (*new_params)(NVSDK_NGX_Parameter** params);
    void                    (*destroy_params)(NVSDK_NGX_Parameter* params);
    NVSDK_NGX_Result        (*create)(void* cmd, NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** feature);
    NVSDK_NGX_Result        (*release)(NVSDK_NGX_Handle* feature);
    void                    (*wait_idle)(void);
    qboolean                feedback;
} dlss5_backend_t;
void DLSS5_SetBackend(const dlss5_backend_t* backend);

/* DLSS5Apply's halves, for a renderer that records the passes and the evaluation itself
   (D3D12: frontend.c, passes.c):
     DLSS5Prepare             whether DLSS 5 runs this frame, at width x height (the output
                              size): its parameter block, and its feature made into `cmd`
                              (or remade - the tuning changed, or the size)
     DLSS5ConvertPush         the encode's (mode 0) or the resolve's (1) push constants
     DLSS5DescribeEvaluation  every evaluation parameter but the resources (DLSSNR.Color,
                              .Depth, .MVec, .Output, and .ControlMask - DLSS5_MASK while
                              DLSS5ControlMask(), else NULL), on the block it returns;
                              *feature is the feature to evaluate. The guides are
                              guideWidth x guideHeight (the render size).
     DLSS5Evaluated           what the evaluation returned: false, and DLSS 5 off until
                              pt_dlss5 is toggled, on a failure (no resolve then) */
qboolean DLSS5Prepare(void* cmd, uint32_t width, uint32_t height);
void DLSS5ConvertPush(int mode, uint32_t width, uint32_t height, qboolean hdr, dlss5_convert_push_t* push);
NVSDK_NGX_Parameter* DLSS5DescribeEvaluation(qboolean resetHistory, uint32_t guideWidth, uint32_t guideHeight,
                                             NVSDK_NGX_Handle** feature);
qboolean DLSS5Evaluated(NVSDK_NGX_Result result);
/* pt_dlss5_volumetric: whether DLSS5_MASK goes in as DLSSNR.ControlMask this frame */
qboolean DLSS5ControlMask(void);

/* The sRGB encode/decode pass around the model (shader/dlss5_convert.comp). */
VkResult vkpt_dlss5_initialize(void);
VkResult vkpt_dlss5_destroy(void);
VkResult vkpt_dlss5_create_pipelines(void);
VkResult vkpt_dlss5_destroy_pipelines(void);
