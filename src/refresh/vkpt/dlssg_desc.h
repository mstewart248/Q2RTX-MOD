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

/* EVERY INPUT TO ONE FRAME GENERATION EVALUATION EXCEPT THE RESOURCES.

   API-free and C, so both renderers share it: DLSS.c fills it
   (DLSSGDescribeEvaluation), and dlssg_ngx.hpp turns it into NGX's
   Opt_Eval parameters for Vulkan (DLSSG.cpp) and D3D12 (d3d12/ngx_dlssg.cpp).
   Includes nothing, so the D3D12 files that take no engine header can too. */

#ifndef VKPT_DLSSG_DESC_H
#define VKPT_DLSSG_DESC_H

typedef struct DLSSGEvalDesc {
    // depth / motionVectors are at RENDER resolution - renderWidth/renderHeight say which.
    unsigned int renderWidth;
    unsigned int renderHeight;

    // Motion vectors here are already normalized to [-1,1] screen units (the Super
    // Resolution path compensates by passing InMVScale = render extent), so DLSS-G
    // needs a scale of 1.0 rather than the render dimensions. Left configurable so
    // the convention can be re-tested rather than re-derived.
    float mvecScaleX;
    float mvecScaleY;

    // WORLD-SPACE CAMERA FRAME. These were never set and NGX's "= {}" left DLSS-G with a
    // camera at the world origin and an all-zero orientation basis, which is not a valid
    // frame at all. Extracted from the inverse view matrix so they cannot disagree with
    // the V/P pair handed over alongside them.
    float cameraPos[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraFwd[3];

    float cameraNear;
    float cameraFar;
    float cameraFOV;          // radians
    float cameraAspectRatio;

    float jitterOffsetX;
    float jitterOffsetY;

    // Row-major 4x4, WITHOUT temporal AA jitter, as NGX requires.
    float cameraViewToClip[16];
    float clipToCameraView[16];
    float clipToPrevClip[16];
    float prevClipToClip[16];

    int colorBuffersHDR;      // our colour is linear HDR pre-tonemap
    int depthInverted;
    int cameraMotionIncluded;
    int reset;                // no usable history this frame
    int notRenderingGameFrames;

    // DEPTH-SPACE TUNING. DLSS-G linearizes the depth buffer itself
    // (lin_depth = 1/depth when depthInverted), and these three are thresholds and a
    // scale IN THAT LINEARIZED SPACE. With the FG depth image holding 1/|d| they are in
    // Quake world units, so the SDK defaults are meaningful here; they are cvars anyway
    // because Quake's world scale is not what NVIDIA tuned against.
    float linearizedDepthScale;             // SDK default 1.0
    float linearizedDepthNearFarPartition;  // SDK default 600.0
    float minRelativeLinearDepthObjectSeparation;   // SDK default 40.0

    // Which value in the motion vector buffer means "invalid". MUST NOT be 0.0 - a pixel
    // that did not move has motion exactly (0,0), so 0.0 marks every static pixel invalid.
    float motionVectorsInvalidValue;

    // Multi Frame Generation. multiFrameCount is the number of GENERATED frames per
    // real frame pair (1 = 2x, 3 = 4x, 5 = 6x); multiFrameIndex is which one of them
    // this call produces, counting from 1. Evaluate is called once per generated frame,
    // each writing its own output image.
    unsigned int multiFrameCount;
    unsigned int multiFrameIndex;

    // UNDOCUMENTED PARAMETERS, TAKEN FROM THE RUNTIME'S OWN STRING TABLE.
    //
    // nvngx_dlssg.dll 310.7.128 accepts a number of DLSSG.* parameters that appear in
    // no public SDK header. Found by dumping the DLL's strings after dxvk-remix turned
    // out to set three parameters (EnableInterp, IsRecording, CmdQueue) that this DLL
    // does NOT contain - Remix targets an older NGX, so its approach does not transfer.
    //
    // indicatorLevel  -> "DLSSG.IndicatorLevel". NVIDIA's on-screen DLSS-G state
    //                    readout. The dev-build DLL draws the same thing, but only onto
    //                    generated frames (we pass no pOutputRealFrame), so it strobes.
    //                    0 = off.
    // streamlineMode  -> "DLSSG.StreamlineMode". The runtime knows whether Streamline is
    //                    driving it, and every title that gets working multi-frame
    //                    generation goes through Streamline while we drive NGX directly.
    //                    That is the one structural difference we have not been able to
    //                    account for. 0 = leave unset (current behaviour).
    //
    // Both are EXPERIMENTS. Zero means "do not set the parameter at all", so the default
    // path is byte-identical to before.
    unsigned int indicatorLevel;
    unsigned int streamlineMode;
} DLSSGEvalDesc;

#endif // VKPT_DLSSG_DESC_H
