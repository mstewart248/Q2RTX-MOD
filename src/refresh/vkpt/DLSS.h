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

typedef struct DLSSRenderResolution DLSSRenderResolution;
typedef struct PrevDlssFeatureValues PrevDlssFeatureValues;
typedef struct DLSSImageHandles DLSSImageHandles;

qboolean DLSSCreated();
const wchar_t* GetWC(const char* c);
void InitDLSSCvars();
qboolean DLSSEnabled();
float GetDLSSResolutionScale();
qboolean CheckSupport();
char* GetFolderPath();
void DLSSPrintCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent);
qboolean DLSSConstructor(VkInstance _instance, VkDevice _device, VkPhysicalDevice _physDevice, const char* _pAppGuid, qboolean _enableDebug);
qboolean TryInit(VkInstance _instance, VkPhysicalDevice _physDevice, const char* _pAppGuid, qboolean _enableDebug);
void DLSSDeconstructor();
void DestroyDLSSFeature();
qboolean IsDLSSAvailable();
NVSDK_NGX_PerfQuality_Value ToNGXPerfQuality();
qboolean AreSameDLSSFeatureValues(struct DLSSRenderResolution resObject);
void SaveDLSSFeatureValues(DLSSRenderResolution resObject);
qboolean ValidateDLSSFeature(VkCommandBuffer cmd, struct DLSSRenderResolution resObject);
NVSDK_NGX_Resource_VK ToNGXResource(VkImage image, VkImageView imageView, NVSDK_NGX_Dimensions size, VkFormat format, bool withWriteAccess);
NVSDK_NGX_Resource_VK ToNGXBufferResource(VkBuffer buffer, size_t bufferSize, bool withWriteAccess);
void DLSSApply(VkCommandBuffer cmd, QVK_t qvk, struct DLSSRenderResolution resObject, vec2 jitterOffset, float timeDelta, qboolean resetAccum);
char* GetDLSSVulkanInstanceExtensions();
char* GetDLSSVulkanDeviceExtensions();
void viewsize_changed(cvar_t* self);
float GetDLSSMultResolutionScale();
qboolean DLSSChanged();
void DLSSSwapChainRecreated();

struct DLSS {	
	VkDevice device;
    qboolean isInitalized;
    NVSDK_NGX_Parameter* pParams;
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