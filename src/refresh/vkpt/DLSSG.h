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

// C-callable shim over DLSS Frame Generation (DLSS-G).
//
// WHY THIS FILE IS C++: unlike the Super Resolution and Ray Reconstruction helpers,
// NVIDIA's DLSS-G headers are C++ only - nvsdk_ngx_params_dlssg.h declares
// NVSDK_NGX_DLSSG_Opt_Eval_Params with C++ default member initializers
// ("unsigned int multiFrameCount = 1;"), which MSVC rejects outright when the
// translation unit is compiled as C. Everything DLSS-G specific is therefore confined
// to DLSSG.cpp and exposed through the plain-C entry points below, so DLSS.c and the
// rest of vkpt stay C.
//
// NVSDK_NGX_Resource_VK / NVSDK_NGX_Parameter / NVSDK_NGX_Handle all come from the
// C-clean headers, so they cross this boundary unchanged.

#pragma once

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>

#include "dlssg_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates the frame generation feature at the given output (post-upscale) resolution.
// Returns the NVSDK_NGX_Result as an unsigned; 0 (NVSDK_NGX_Result_Success) on success.
// width/height are the OUTPUT (post-upscale) size; renderWidth/renderHeight are the
// size of the depth and motion-vector buffers, which differ whenever DLSS is upscaling.
// The render pair is the LARGEST extent it will be fed (DynamicResolutionScaling,
// dlssg_ngx.hpp); feeding it more than that faults inside NGX.
// multiFrameCount is the number of GENERATED frames per real frame pair (1 = 2x,
// 2 = 3x, 3 = 4x). It is passed at CREATE time as well as per-Evaluate: see the note
// in DLSSG_CreateFeature for why the per-Evaluate value alone is not enough.
unsigned int DLSSG_CreateFeature(VkCommandBuffer cmd,
                                 struct NVSDK_NGX_Parameter* pParams,
                                 unsigned int width,
                                 unsigned int height,
                                 unsigned int renderWidth,
                                 unsigned int renderHeight,
                                 VkFormat backbufferFormat,
                                 unsigned int multiFrameCount,
                                 struct NVSDK_NGX_Handle** ppOutHandle);

// Inputs for one frame generation evaluation.
//
// backbuffer / outputInterpolated are at OUTPUT (post-upscale) resolution.
// depth / motionVectors are at RENDER resolution - desc->renderWidth/renderHeight say which.
typedef struct DLSSG_EvalInputs {
    NVSDK_NGX_Resource_VK* pBackbuffer;
    NVSDK_NGX_Resource_VK* pDepth;
    NVSDK_NGX_Resource_VK* pMotionVectors;
    NVSDK_NGX_Resource_VK* pOutputInterpolated;

    // Optional. The scene colour WITHOUT any HUD. Q2RTX draws its 2D layer onto the
    // swapchain images after frame generation, so the backbuffer handed over is already
    // HUD-less and this can simply alias it; NULL leaves the runtime to detect UI itself.
    NVSDK_NGX_Resource_VK* pHudless;

    // Everything else, which the D3D12 renderer hands over too: see dlssg_desc.h
    // (DLSSGDescribeEvaluation fills it).
    const DLSSGEvalDesc* desc;
} DLSSG_EvalInputs;

// Returns the NVSDK_NGX_Result as an unsigned; 0 on success.
unsigned int DLSSG_EvaluateFeature(VkCommandBuffer cmd,
                                   struct NVSDK_NGX_Handle* pHandle,
                                   struct NVSDK_NGX_Parameter* pParams,
                                   const DLSSG_EvalInputs* pInputs);

#ifdef __cplusplus
}
#endif
