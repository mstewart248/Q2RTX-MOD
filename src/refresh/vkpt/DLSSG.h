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

#ifdef __cplusplus
extern "C" {
#endif

// Creates the frame generation feature at the given output (post-upscale) resolution.
// Returns the NVSDK_NGX_Result as an unsigned; 0 (NVSDK_NGX_Result_Success) on success.
// width/height are the OUTPUT (post-upscale) size; renderWidth/renderHeight are the
// size of the depth and motion-vector buffers, which differ whenever DLSS is upscaling.
// Getting the render pair wrong is not cosmetic: the feature is created with
// DynamicResolutionScaling = false, so DLSS-G treats them as a fixed promise.
unsigned int DLSSG_CreateFeature(VkCommandBuffer cmd,
                                 struct NVSDK_NGX_Parameter* pParams,
                                 unsigned int width,
                                 unsigned int height,
                                 unsigned int renderWidth,
                                 unsigned int renderHeight,
                                 VkFormat backbufferFormat,
                                 struct NVSDK_NGX_Handle** ppOutHandle);

// Inputs for one frame generation evaluation.
//
// backbuffer / outputInterpolated are at OUTPUT (post-upscale) resolution.
// depth / motionVectors are at RENDER resolution - renderWidth/renderHeight say which.
typedef struct DLSSG_EvalInputs {
    NVSDK_NGX_Resource_VK* pBackbuffer;
    NVSDK_NGX_Resource_VK* pDepth;
    NVSDK_NGX_Resource_VK* pMotionVectors;
    NVSDK_NGX_Resource_VK* pOutputInterpolated;

    unsigned int renderWidth;
    unsigned int renderHeight;

    // Motion vectors here are already normalized to [-1,1] screen units (the Super
    // Resolution path compensates by passing InMVScale = render extent), so DLSS-G
    // needs a scale of 1.0 rather than the render dimensions. Left configurable so
    // the convention can be re-tested rather than re-derived.
    float mvecScaleX;
    float mvecScaleY;

    float cameraNear;
    float cameraFar;
    float cameraFOV;          // radians
    float cameraAspectRatio;

    float jitterOffsetX;
    float jitterOffsetY;

    // Row-major 4x4, WITHOUT temporal AA jitter, as NGX requires.
    const float* pCameraViewToClip;
    const float* pClipToCameraView;
    const float* pClipToPrevClip;
    const float* pPrevClipToClip;

    int colorBuffersHDR;      // our colour is linear HDR pre-tonemap
    int depthInverted;
    int cameraMotionIncluded;
    int reset;                // no usable history this frame
    int notRenderingGameFrames;

    // Multi Frame Generation. multiFrameCount is the number of GENERATED frames per
    // real frame pair (1 = 2x, 3 = 4x, 5 = 6x); multiFrameIndex is which one of them
    // this call produces, counting from 1. Evaluate is called once per generated frame,
    // each writing its own output image.
    unsigned int multiFrameCount;
    unsigned int multiFrameIndex;
} DLSSG_EvalInputs;

// Returns the NVSDK_NGX_Result as an unsigned; 0 on success.
unsigned int DLSSG_EvaluateFeature(VkCommandBuffer cmd,
                                   struct NVSDK_NGX_Handle* pHandle,
                                   struct NVSDK_NGX_Parameter* pParams,
                                   const DLSSG_EvalInputs* pInputs);

#ifdef __cplusplus
}
#endif
