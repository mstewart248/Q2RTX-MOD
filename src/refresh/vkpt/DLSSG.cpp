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

// DLSS Frame Generation (DLSS-G), isolated in C++.
//
// nvsdk_ngx_params_dlssg.h uses C++ default member initializers, so it cannot be
// included from a .c file - see the comment in DLSSG.h. This TU is the only place
// that touches the DLSS-G helper headers.

#include <string.h>

#include <nvsdk_ngx_helpers_dlssg_vk.h>

#include "DLSSG.h"
#include "dlssg_ngx.hpp"

extern "C" unsigned int DLSSG_CreateFeature(VkCommandBuffer cmd,
                                            NVSDK_NGX_Parameter* pParams,
                                            unsigned int width,
                                            unsigned int height,
                                            unsigned int renderWidth,
                                            unsigned int renderHeight,
                                            VkFormat backbufferFormat,
                                            unsigned int multiFrameCount,
                                            NVSDK_NGX_Handle** ppOutHandle)
{
    // the parameters, as the D3D12 renderer makes them too (dlssg_ngx.hpp)
    NVSDK_NGX_DLSSG_Create_Params createParams = {};
    DLSSG_FillCreateParams(pParams, width, height, renderWidth, renderHeight,
                           (unsigned int)backbufferFormat, multiFrameCount, &createParams);

    // Only one physical device.
    const unsigned int creationNodeMask = 1;
    const unsigned int visibilityNodeMask = 1;

    return (unsigned int)NGX_VK_CREATE_DLSSG(cmd, creationNodeMask, visibilityNodeMask,
                                             ppOutHandle, pParams, &createParams);
}

extern "C" unsigned int DLSSG_EvaluateFeature(VkCommandBuffer cmd,
                                              NVSDK_NGX_Handle* pHandle,
                                              NVSDK_NGX_Parameter* pParams,
                                              const DLSSG_EvalInputs* in)
{
    NVSDK_NGX_VK_DLSSG_Eval_Params evalParams = {};
    evalParams.pBackbuffer = in->pBackbuffer;
    evalParams.pDepth = in->pDepth;
    evalParams.pMVecs = in->pMotionVectors;
    evalParams.pOutputInterpFrame = in->pOutputInterpolated;
    // HUDLess: the backbuffer we hand over is already the pre-HUD image (the 2D layer
    // is drawn onto the swapchain after frame generation), so the caller can pass the
    // same resource here. That tells the runtime there is no UI in the frame; with
    // both HUDLess and UI absent it runs its own UI-detection heuristics instead.
    evalParams.pHudless = in->pHudless;
    evalParams.pUI = NULL;
    evalParams.pUIAlpha = NULL;
    evalParams.pBidirectionalDistortionField = NULL;
    evalParams.pOutputRealFrame = NULL;
    evalParams.pOutputDisableInterpolation = NULL;

    // everything but the resources, as the D3D12 renderer hands it over too
    // (dlssg_ngx.hpp)
    NVSDK_NGX_DLSSG_Opt_Eval_Params opt = {};
    DLSSG_FillOptEvalParams(in->desc, pParams, &opt);

    return (unsigned int)NGX_VK_EVALUATE_DLSSG(cmd, pHandle, pParams, &evalParams, &opt);
}
