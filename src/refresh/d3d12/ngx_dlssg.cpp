/*
DLSS FRAME GENERATION THROUGH D3D12, as vkpt/DLSSG.cpp runs it through NGX's Vulkan
entry points: the feature and one evaluation. What to hand it is DLSS.c's
(DLSSGDescribeEvaluation, called from frontend.c), and it reaches NGX through the same
code as Vulkan's (vkpt/dlssg_ngx.hpp); only the resources and the entry points are this
API's. C++, because NGX's DLSS-G parameter structs have default member initializers.
Engine-free like ngx.c, so tests/d3d12_world_test.c builds it beside and runs it.

RESOURCE STATES. As with Super Resolution (ngx.h): the inputs readable by a compute
shader, the output in UNORDERED_ACCESS. The images are simultaneous-access textures
(world.c), so they are handed over as they are.

COMMAND LIST STATE. An evaluation sets NGX's own descriptor heaps, root signature and
pipeline on the list, as Super Resolution's does: the caller sets its own again.
*/

#include <d3d12.h>

#include <nvsdk_ngx_helpers_dlssg.h>

#include "../vkpt/dlssg_ngx.hpp"
#include "ngx.h"

extern "C" NVSDK_NGX_Result d3d12_ngx_dlssg_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Parameter *params,
                                                   uint32_t width, uint32_t height,
                                                   uint32_t render_width, uint32_t render_height,
                                                   DXGI_FORMAT backbuffer_format, uint32_t generated_frames,
                                                   NVSDK_NGX_Handle **feature)
{
    NVSDK_NGX_DLSSG_Create_Params create = {};
    DLSSG_FillCreateParams(params, width, height, render_width, render_height, (unsigned int)backbuffer_format,
                           generated_frames, &create);

    // one GPU: node 1 makes it and sees it
    return NGX_D3D12_CREATE_DLSSG(cmd, 1, 1, feature, params, &create);
}

extern "C" NVSDK_NGX_Result d3d12_ngx_dlssg_evaluate(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *feature,
                                                     NVSDK_NGX_Parameter *params, const d3d12_ngx_dlssg_eval_t *e)
{
    // DLSSG.cpp's resources, in this API's form
    NVSDK_NGX_D3D12_DLSSG_Eval_Params ev = {};
    ev.pBackbuffer = e->backbuffer;
    ev.pDepth = e->depth;
    ev.pMVecs = e->motion;
    ev.pHudless = e->hudless;
    ev.pUI = NULL;
    ev.pUIAlpha = NULL;
    ev.pBidirectionalDistortionField = NULL;
    ev.pOutputInterpFrame = e->output;
    ev.pOutputRealFrame = NULL;
    ev.pOutputDisableInterpolation = NULL;

    NVSDK_NGX_DLSSG_Opt_Eval_Params opt = {};
    DLSSG_FillOptEvalParams(e->desc, params, &opt);

    // DLSSGApply sets it once a frame; it is the same value for each of the frame's
    NVSDK_NGX_Parameter_SetULL(params, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, (unsigned long long)e->frame_id);

    return NGX_D3D12_EVALUATE_DLSSG(cmd, feature, params, &ev, &opt);
}
