/*
Copyright (C) 2026 Matt Stewart

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/* THE API-FREE HALF OF DLSSG.cpp: the create parameters and the Opt_Eval
   parameters, which both renderers hand to NGX - Vulkan through
   NGX_VK_CREATE/EVALUATE_DLSSG (DLSSG.cpp), D3D12 through
   NGX_D3D12_CREATE/EVALUATE_DLSSG (d3d12/ngx_dlssg.cpp).

   C++: nvsdk_ngx_params_dlssg.h gives its structs default member initializers.
   Include it after either API's nvsdk_ngx_helpers_dlssg header, which brings the
   NGX parameter types. Engine-free, like dlssg_desc.h. */

#ifndef VKPT_DLSSG_NGX_HPP
#define VKPT_DLSSG_NGX_HPP

#include <string.h>

#include "dlssg_desc.h"

static inline void DLSSG_CopyMatrix(float dst[4][4], const float* src)
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

/* The create parameters, and the ones NGX reads from the block at create time.
   `backbufferFormat` is the API's own format value (a VkFormat or a DXGI_FORMAT).
   renderWidth/Height are the LARGEST render extent the feature will be fed (both
   callers pass DLSSFeatureResolution's), and DynamicResolutionScaling says so: each
   evaluation names the frame's own extent as the depth and motion vectors' subrect
   (DLSSG_FillOptEvalParams). Until 11-06 it was false and the frame's extent, so every
   DRS step remade the feature behind an idle wait. */
static inline void DLSSG_FillCreateParams(NVSDK_NGX_Parameter* pParams,
                                          unsigned int width,
                                          unsigned int height,
                                          unsigned int renderWidth,
                                          unsigned int renderHeight,
                                          unsigned int backbufferFormat,
                                          unsigned int multiFrameCount,
                                          NVSDK_NGX_DLSSG_Create_Params* createParams)
{
    createParams->Width = width;
    createParams->Height = height;
    createParams->NativeBackbufferFormat = backbufferFormat;
    createParams->RenderWidth = renderWidth;
    createParams->RenderHeight = renderHeight;
    createParams->DynamicResolutionScaling = true;

    // NGX_VK_CREATE_DLSSG only sets the generic NVSDK_NGX_Parameter_Width/Height, which
    // the snippet reports as deprecated ("please use NVSDK_NGX_DLSSG_Parameter_Width/
    // Height instead"). Setting the DLSS-G specific pair here takes precedence.
    NVSDK_NGX_Parameter_SetUI(pParams, NVSDK_NGX_DLSSG_Parameter_Width, width);
    NVSDK_NGX_Parameter_SetUI(pParams, NVSDK_NGX_DLSSG_Parameter_Height, height);

    /* MULTI FRAME GENERATION HAS TO BE DECLARED AT CREATE TIME, NOT ONLY PER EVALUATE.
       MEASURED 2026-09-02: with MultiFrameCount/MultiFrameIndex set only in the
       Opt_Eval params, every generated frame came back IDENTICAL - byte-identical in
       two captures - and every one of them sat at fractional position ~0.50 between the
       two real frames. 0.50 is the 2x answer. Three evenly spaced frames would read
       0.25 / 0.50 / 0.75, and if the three Evaluates had merely raced on the parameter
       they would all read 0.75 (the last index written). Landing exactly on the 2x
       midpoint means the feature was created as a 2x feature and ignored the
       per-Evaluate index entirely.
       That is why 2x always looked right and 3x/4x read as "hold, hold, hold, JUMP".
       DLSS-G does read NGX parameters at create time beyond the create struct - the
       runtime log prints UserInterfaceRecompositionEnabled being parsed there - and
       NVSDK_NGX_DLSSG_Create_Params has no field for this, so the parameter block is
       the only route.
       Because it is create-time, the feature MUST be recreated when the multiplier
       changes; ValidateDLSSGFeature tracks it alongside the extents for that reason. */
    NVSDK_NGX_Parameter_SetUI(pParams, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,
                              multiFrameCount ? multiFrameCount : 1);
}

/* The Opt_Eval parameters for one evaluation, and the two raw parameters that go on the
   block beside them. */
static inline void DLSSG_FillOptEvalParams(const DLSSGEvalDesc* in,
                                           NVSDK_NGX_Parameter* pParams,
                                           NVSDK_NGX_DLSSG_Opt_Eval_Params* optOut)
{
    // NOTE: every field below is set explicitly rather than relying on the header's
    // C++ default member initializers - "= {}" does not necessarily preserve them,
    // and minRelativeLinearDepthObjectSeparation defaulting to 0 instead of 40 would
    // silently change the result.
    NVSDK_NGX_DLSSG_Opt_Eval_Params& opt = *optOut;

    // 1 = 2x, 3 = 4x, 5 = 6x. The caller clamps these to the driver's
    // DLSSG.MultiFrameCountMax; guard anyway so a zero can never reach NGX.
    opt.multiFrameCount = in->multiFrameCount ? in->multiFrameCount : 1;
    opt.multiFrameIndex = in->multiFrameIndex ? in->multiFrameIndex : 1;

    DLSSG_CopyMatrix(opt.cameraViewToClip, in->cameraViewToClip);
    DLSSG_CopyMatrix(opt.clipToCameraView, in->clipToCameraView);
    DLSSG_CopyMatrix(opt.clipToLensClip, NULL);           // no lens distortion
    DLSSG_CopyMatrix(opt.clipToPrevClip, in->clipToPrevClip);
    DLSSG_CopyMatrix(opt.prevClipToClip, in->prevClipToClip);

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
    // HUDLess covers its whole resource, like the backbuffer.
    opt.hudLessSubrectBase = { 0, 0 };
    opt.hudLessSubrectSize = { 0, 0 };

    opt.minRelativeLinearDepthObjectSeparation = in->minRelativeLinearDepthObjectSeparation;

    // Raw NGX parameters - these two are NOT members of Opt_Eval_Params, they are set
    // on pParams directly. See the comment block above
    // NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale in nvsdk_ngx_defs_dlssg.h.
    NVSDK_NGX_Parameter_SetF(pParams, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale,
                             in->linearizedDepthScale);
    NVSDK_NGX_Parameter_SetF(pParams, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_NearFarPartition,
                             in->linearizedDepthNearFarPartition);

    /* Undocumented parameters - see the note in dlssg_desc.h. Set by literal string
       because they exist in the runtime's string table but in no SDK header we have; NGX
       parameters are string-keyed, so this is the same mechanism the macros use. A name
       the runtime does not know is ignored, which is why an unset default is safe. */
    if (in->indicatorLevel)
        NVSDK_NGX_Parameter_SetUI(pParams, "DLSSG.IndicatorLevel", in->indicatorLevel);
    if (in->streamlineMode)
        NVSDK_NGX_Parameter_SetUI(pParams, "DLSSG.StreamlineMode", in->streamlineMode);
}

#endif // VKPT_DLSSG_NGX_HPP
