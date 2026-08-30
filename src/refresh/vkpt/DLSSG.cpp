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

static void copy_matrix(float dst[4][4], const float* src)
{
    if (src) {
        memcpy(dst, src, sizeof(float) * 16);
    }
    else {
        // An all-zero matrix would be actively wrong (it collapses every position to
        // the origin); identity at least means "no transform".
        memset(dst, 0, sizeof(float) * 16);
        dst[0][0] = dst[1][1] = dst[2][2] = dst[3][3] = 1.0f;
    }
}

extern "C" unsigned int DLSSG_CreateFeature(VkCommandBuffer cmd,
                                            NVSDK_NGX_Parameter* pParams,
                                            unsigned int width,
                                            unsigned int height,
                                            unsigned int renderWidth,
                                            unsigned int renderHeight,
                                            VkFormat backbufferFormat,
                                            NVSDK_NGX_Handle** ppOutHandle)
{
    NVSDK_NGX_DLSSG_Create_Params createParams = {};
    createParams.Width = width;
    createParams.Height = height;
    createParams.NativeBackbufferFormat = (unsigned int)backbufferFormat;
    createParams.RenderWidth = renderWidth;
    createParams.RenderHeight = renderHeight;
    createParams.DynamicResolutionScaling = false;

    // NGX_VK_CREATE_DLSSG only sets the generic NVSDK_NGX_Parameter_Width/Height, which
    // the snippet reports as deprecated ("please use NVSDK_NGX_DLSSG_Parameter_Width/
    // Height instead"). Setting the DLSS-G specific pair here takes precedence.
    NVSDK_NGX_Parameter_SetUI(pParams, NVSDK_NGX_DLSSG_Parameter_Width, width);
    NVSDK_NGX_Parameter_SetUI(pParams, NVSDK_NGX_DLSSG_Parameter_Height, height);

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
    // HUDLess / UI are left null for now: the backbuffer we hand over is already the
    // pre-HUD image, so there is no UI for DLSS-G to separate out yet.
    evalParams.pHudless = NULL;
    evalParams.pUI = NULL;
    evalParams.pUIAlpha = NULL;
    evalParams.pBidirectionalDistortionField = NULL;
    evalParams.pOutputRealFrame = NULL;
    evalParams.pOutputDisableInterpolation = NULL;

    // NOTE: every field below is set explicitly rather than relying on the header's
    // C++ default member initializers - "= {}" does not necessarily preserve them,
    // and minRelativeLinearDepthObjectSeparation defaulting to 0 instead of 40 would
    // silently change the result.
    NVSDK_NGX_DLSSG_Opt_Eval_Params opt = {};

    // 1 = 2x, 3 = 4x, 5 = 6x. The caller clamps these to the driver's
    // DLSSG.MultiFrameCountMax; guard anyway so a zero can never reach NGX.
    opt.multiFrameCount = in->multiFrameCount ? in->multiFrameCount : 1;
    opt.multiFrameIndex = in->multiFrameIndex ? in->multiFrameIndex : 1;

    copy_matrix(opt.cameraViewToClip, in->pCameraViewToClip);
    copy_matrix(opt.clipToCameraView, in->pClipToCameraView);
    copy_matrix(opt.clipToLensClip, NULL);           // no lens distortion
    copy_matrix(opt.clipToPrevClip, in->pClipToPrevClip);
    copy_matrix(opt.prevClipToClip, in->pPrevClipToClip);

    opt.jitterOffset[0] = in->jitterOffsetX;
    opt.jitterOffset[1] = in->jitterOffsetY;

    opt.mvecScale[0] = in->mvecScaleX;
    opt.mvecScale[1] = in->mvecScaleY;

    opt.cameraPinholeOffset[0] = 0.0f;
    opt.cameraPinholeOffset[1] = 0.0f;

    for (int i = 0; i < 3; i++) {
        opt.cameraPos[i]   = in->cameraPos[i];
        opt.cameraUp[i]    = in->cameraUp[i];
        opt.cameraRight[i] = in->cameraRight[i];
        opt.cameraFwd[i]   = in->cameraFwd[i];
    }

    opt.cameraNear = in->cameraNear;
    opt.cameraFar = in->cameraFar;
    opt.cameraFOV = in->cameraFOV;
    opt.cameraAspectRatio = in->cameraAspectRatio;

    opt.colorBuffersHDR = in->colorBuffersHDR ? true : false;
    opt.depthInverted = in->depthInverted ? true : false;
    opt.cameraMotionIncluded = in->cameraMotionIncluded ? true : false;
    opt.reset = in->reset ? true : false;
    opt.automodeOverrideReset = false;
    opt.notRenderingGameFrames = in->notRenderingGameFrames ? true : false;
    opt.orthoProjection = false;

    // NOT 0.0f. This field names the value that means "invalid" in the motion vector
    // buffer, and a pixel that did not move has motion exactly (0,0) - so 0.0 declared
    // every STATIC pixel's motion vector invalid and pushed DLSS-G onto optical flow
    // for most of the screen. Streamline's default is FLT_MIN, a value no real vector
    // takes. The field has no default member initializer in nvsdk_ngx_params_dlssg.h,
    // so "= {}" zeroed it and the explicit 0.0f merely restated the zero.
    opt.motionVectorsInvalidValue = in->motionVectorsInvalidValue;
    opt.motionVectorsDilated = false;
    opt.menuDetectionEnabled = false;

    // Depth and motion vectors are at render resolution; the colour buffers are at
    // output resolution. Everything else covers its whole resource, which {0,0} means.
    opt.mvecsSubrectBase = { 0, 0 };
    opt.mvecsSubrectSize = { in->renderWidth, in->renderHeight };
    opt.depthSubrectBase = { 0, 0 };
    opt.depthSubrectSize = { in->renderWidth, in->renderHeight };

    opt.minRelativeLinearDepthObjectSeparation = in->minRelativeLinearDepthObjectSeparation;

    // Raw NGX parameters - these two are NOT members of Opt_Eval_Params, they are set
    // on pParams directly. See the comment block above
    // NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale in nvsdk_ngx_defs_dlssg.h.
    NVSDK_NGX_Parameter_SetF(pParams, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale,
                             in->linearizedDepthScale);
    NVSDK_NGX_Parameter_SetF(pParams, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_NearFarPartition,
                             in->linearizedDepthNearFarPartition);

    return (unsigned int)NGX_VK_EVALUATE_DLSSG(cmd, pHandle, pParams, &evalParams, &opt);
}
