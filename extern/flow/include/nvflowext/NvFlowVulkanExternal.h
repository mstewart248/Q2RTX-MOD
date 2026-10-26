/*
Q2RTX addition to the NVIDIA Flow SDK (BSD-3, see extern/flow/LICENSE.md).

Runs Flow's Vulkan backend on a VkDevice the application already owns, instead
of the private instance and device the reference backend creates for itself.
setup/build_flow.py copies this header and ExternalVulkan.cpp into the Flow
source tree before building nvflowext.dll, and stages the header in
extern/flow/include/nvflowext for the game.

Why: with its own device, Flow's results can only cross to the renderer as
exported buffers (no textures) plus cross-device semaphores. On the shared
device Flow's command buffers go to the renderer's queue, so plain submission
order and a pipeline barrier are all the synchronisation there is, and the
renderer samples Flow's density atlas directly.

The application keeps ownership: destroying the Flow device or device manager
leaves the VkDevice and VkInstance alone. Flow submits to `queue` itself, so
if anything else submits to that queue from another thread, pass a
getInstanceProcAddr that hands Flow a vkQueueSubmit / vkQueueWaitIdle which
take the application's queue lock.
*/

#ifndef NV_FLOW_VULKAN_EXTERNAL_H
#define NV_FLOW_VULKAN_EXTERNAL_H

#include "NvFlowExt.h"

typedef struct NvFlowVulkanExternalDesc
{
	void* instance;                 // VkInstance
	void* physicalDevice;           // VkPhysicalDevice
	void* device;                   // VkDevice
	void* queue;                    // VkQueue Flow submits to
	NvFlowUint queueFamilyIndex;    // family of `queue`, graphics or compute
	void* getInstanceProcAddr;      // PFN_vkGetInstanceProcAddr
	NvFlowBool32 shaderStorageImageWriteWithoutFormat; // enabled on `device`
	NvFlowLogPrint_t logPrint;
} NvFlowVulkanExternalDesc;

typedef struct NvFlowVulkanExternalTexture
{
	void* image;                    // VkImage
	void* imageView;                // VkImageView over every mip level
	NvFlowUint layout;              // VkImageLayout between Flow frames
} NvFlowVulkanExternalTexture;

typedef struct NvFlowVulkanExternalBuffer
{
	void* buffer;                   // VkBuffer
	NvFlowUint64 sizeInBytes;
} NvFlowVulkanExternalBuffer;

/* Exported by nvflowext as NvFlowVulkanExternalCreateDeviceManager etc. Use the
   device manager and device with the ordinary NvFlowDeviceInterface from
   NvFlowGetDeviceInterface(eNvFlowContextApi_vulkan): getDeviceQueue,
   getContext, flush, waitIdle, destroyDevice, destroyDeviceManager. */
typedef NvFlowDeviceManager*(NV_FLOW_ABI* PFN_NvFlowVulkanExternalCreateDeviceManager)(const NvFlowVulkanExternalDesc* desc);
typedef NvFlowDevice*(NV_FLOW_ABI* PFN_NvFlowVulkanExternalCreateDevice)(NvFlowDeviceManager* manager, const NvFlowVulkanExternalDesc* desc);

/* The native handles behind a persistent texture or buffer from
   NvFlowContextInterface::createTexture / createBuffer. Between Flow frames a
   texture sits in `layout` (SHADER_READ_ONLY_OPTIMAL for Flow's own). */
typedef void(NV_FLOW_ABI* PFN_NvFlowVulkanExternalGetTexture)(NvFlowTexture* texture, NvFlowVulkanExternalTexture* out);
typedef void(NV_FLOW_ABI* PFN_NvFlowVulkanExternalGetBuffer)(NvFlowBuffer* buffer, NvFlowVulkanExternalBuffer* out);

#endif
