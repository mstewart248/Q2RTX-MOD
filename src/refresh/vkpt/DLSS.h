// Copyright (c) 2021 Matt Stewart
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

#pragma once

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <nvsdk_ngx_defs_dlssg.h>

#include "dlssg_desc.h"


#include "shared/shared.h"
#include "vkpt.h"
#include "common/cvar.h"

#define API_VERSION 3

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;
struct DLSSRenderResolution {
    int inputWidth;
    int inputHeight;
    int outputWidth;
    int outputHeight;
};
struct PrevDlssFeatureValues
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t upscaledWidth;
    uint32_t upscaledHeight;
};
struct DLSSImageHandles {
    VkImage image;
    VkImageView imageView;
    VkFormat format;
};

/* pt_dlss_debug's image (VKPT_IMG_*), shown raw in place of the picture by both
   renderers, or -1 when off or not a view. `pair` (may be NULL) is set for an A/B pair,
   whose _A this is: Vulkan shows _A + (frame_counter & 1), D3D12 its binding's image. The
   table is in DLSS.c; declared again in d3d12/d3d12pt.h. */
int DLSSDebugImage(int* pair);

/* What an evaluation hands DLSS beyond the fixed images (DLSSDescribeEvaluation): the
   colour image (always TAA_OUTPUT now), the jitter in render pixels, which of
   Ray Reconstruction's optional guides go over, and its matrices (pt_dlss_spec_motion). */
typedef struct {
    int         color_image;            // VKPT_IMG_*
    float       jitter_x, jitter_y;
    qboolean    diffuse_hit_distance;   // pt_dlss_diff_hitdist
    qboolean    reflected_albedo;       // pt_dlss_reflected_albedo
    qboolean    before_transparency;    // pt_dlss_bt_guide != 2
    qboolean    reflection_motion;      // DLSS_REFLECT_MOTION as the reflections' motion
    qboolean    matrices;               // world_to_view and view_to_clip go over
    float       world_to_view[16], view_to_clip[16];
} DLSSEvaluation;

// NGX's project id for this game, for either API's init
#define DLSS_PROJECT_ID "FFA5FAF5-2329-44AB-A423-3D9B3B177C88"

typedef struct DLSSRenderResolution DLSSRenderResolution;
typedef struct PrevDlssFeatureValues PrevDlssFeatureValues;
typedef struct DLSSImageHandles DLSSImageHandles;

qboolean DLSSCreated();
/* The capability block of the NGX instance TryInit set up, when that instance is up on
   `device`; NULL otherwise. DLSS 5 (DLSSNR.c) shares the instance and reads DLSSNR.* from
   it. */
NVSDK_NGX_Parameter* DLSSCapabilityParameters(VkDevice device);
const wchar_t* GetWC(const char* c);
void InitDLSSCvars();
qboolean DLSSEnabled();
float GetDLSSResolutionScale();
qboolean CheckSupport();
void ReportFrameGenSupport();
char* GetFolderPath();
void DLSSPrintCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent);
qboolean DLSSConstructor(VkInstance _instance, VkDevice _device, VkPhysicalDevice _physDevice, const char* _pAppGuid, qboolean _enableDebug);
qboolean TryInit(VkInstance _instance, VkPhysicalDevice _physDevice, const char* _pAppGuid, qboolean _enableDebug);
void DLSSDeconstructor();
void DestroyDLSSFeature();
qboolean IsDLSSAvailable();
void DLSSPublishAvailability(void);
NVSDK_NGX_PerfQuality_Value ToNGXPerfQuality();
qboolean AreSameDLSSFeatureValues(struct DLSSRenderResolution resObject);
void SaveDLSSFeatureValues(DLSSRenderResolution resObject);
qboolean ValidateDLSSFeature(VkCommandBuffer cmd, struct DLSSRenderResolution resObject);
/* The API-free halves of ValidateDLSSFeature and DLSSApply, which the D3D12 renderer
   (d3d12/frontend.c) calls too - see each in DLSS.c. */
qboolean DLSSAdoptParameters(NVSDK_NGX_Parameter* params);
void DLSSClampResolution(struct DLSSRenderResolution* resObject);
// The feature's resolution for a frame's (clamped) one: made for the largest render
// extent, evaluated over the frame's as a sub-rectangle - DRS and photo mode remake nothing
void DLSSFeatureResolution(struct DLSSRenderResolution* resObject);
qboolean DLSSDescribeFeature(struct DLSSRenderResolution resObject, NVSDK_NGX_Parameter* params,
                             NVSDK_NGX_DLSS_Create_Params* sr, NVSDK_NGX_DLSSD_Create_Params* rr);
void DLSSDescribeEvaluation(const vec2 jitterOffset, DLSSEvaluation* ev);
void DLSSFeatureFailed(void);
void DLSSModeSelected(cvar_t* self);
NVSDK_NGX_Resource_VK ToNGXResource(VkImage image, VkImageView imageView, NVSDK_NGX_Dimensions size, VkFormat format, bool withWriteAccess);
NVSDK_NGX_Resource_VK ToNGXBufferResource(VkBuffer buffer, size_t bufferSize, bool withWriteAccess);
void DLSSApply(VkCommandBuffer cmd, QVK_t qvk, struct DLSSRenderResolution resObject, vec2 jitterOffset, float timeDelta, qboolean resetAccum);
char* GetDLSSVulkanInstanceExtensions();
char* GetDLSSVulkanDeviceExtensions();
void viewsize_changed(cvar_t* self);
void DlssModeChanged(cvar_t* self);
void DlssSplitFieldsChanged(cvar_t* self);
void DlssFieldResChanged(cvar_t* self);
qboolean DLSSSplitFieldsEnabled();
qboolean DLSSFieldHalfRes();
qboolean DLSSRayReconstructionActive();
qboolean DLSSBypassDenoiser();
int DLSSMode();
int DLSSModeDenoise();
float GetDLSSMultResolutionScale();
qboolean DLSSChanged();
void DLSSSwapChainRecreated();

/* DLSS Frame Generation (DLSS-G). See the driver block at the end of DLSS.c. */

void InitDLSSGCvars();
qboolean DLSSGEnabled();
/* Effective multiplier: 0 when off, otherwise 2..(driver max + 1). Already clamped to
   what the driver reported in DLSSG.MultiFrameCountMax, so callers can trust it. */
unsigned int DLSSGMultiplier();
/* Generated (interpolated) frames per real frame = multiplier - 1, or 0 when off. */
unsigned int DLSSGGeneratedFrames();

/* Per-frame ceiling on the number of GENERATED frames, set from how many presents the
   display can actually show in one render interval. 0 = no limit (use the multiplier the
   user asked for). Set it BEFORE DLSSGApply so the evaluate loop, the swapchain acquire
   and the present schedule all agree on the count. */
void DLSSGSetFrameBudget(unsigned int maxGeneratedFrames);
/* Highest multiplier this device supports, for the menu to bound its choices. */
unsigned int DLSSGMaxMultiplier();
qboolean DLSSGShowInterpolated();
/* True once the DLSS-G feature actually exists, i.e. an interpolated image has been
   produced and is safe to present. False on the first FG frame and after a failure. */
qboolean DLSSGFeatureReady();
/* Interpolated output for generated frame `generatedIndex`, counting from 1. */
VkImage GetDLSSGImage(unsigned int generatedIndex);
// the VKPT_IMG_DLSS_FG_OUTPUT* image generated frame `generatedIndex` (1..) is written to
int DLSSGOutputImageIndex(unsigned int generatedIndex);
void DLSSGApply(VkCommandBuffer cmd, qboolean resetAccum);
void DestroyDLSSGFeature();

/* The API-free half of DLSSGApply, which the D3D12 renderer (d3d12/frontend.c) calls
   too: every evaluation input but the resources. */
typedef struct {
    DLSSGEvalDesc   desc;               // multiFrameIndex 1: the caller sets it per frame
    int             depth_image;        // VKPT_IMG_DLSS_FG_DEPTH (DLSS_DEPTH: pt_dlss_fg_depthmode 0)
    qboolean        hudless;            // hand the backbuffer over as the HUD-less colour too
    unsigned int    generated_frames;   // DLSSGGeneratedFrames(): the evaluations to run
} DLSSGEvaluation;
void DLSSGDescribeEvaluation(qboolean resetAccum, DLSSGEvaluation* ev);
// the D3D12 renderer's create or evaluate failed: frame generation stays off
void DLSSGFeatureFailed(void);

struct DLSS {	
	VkDevice device;
    qboolean isInitalized;
    NVSDK_NGX_Parameter* pParams;
	NVSDK_NGX_DLSSD_Create_Params* pDlssdParams;
    NVSDK_NGX_Handle* pDlssFeature;
    PrevDlssFeatureValues prevDlssFeatureValues;
	qboolean created;
};

typedef struct DLSS DLSS;

#define BARRIER_COMPUTE(cmd_buf, img) \
	do { \
		VkImageSubresourceRange subresource_range = { \
			.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT, \
			.baseMipLevel   = 0, \
			.levelCount     = 1, \
			.baseArrayLayer = 0, \
			.layerCount     = 1 \
		}; \
		IMAGE_BARRIER(cmd_buf, \
				.image            = img, \
				.subresourceRange = subresource_range, \
				.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT, \
				.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT, \
				.oldLayout        = VK_IMAGE_LAYOUT_GENERAL, \
				.newLayout        = VK_IMAGE_LAYOUT_GENERAL, \
		); \
	} while(0)