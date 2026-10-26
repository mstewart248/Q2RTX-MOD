// Q2RTX addition to the NVIDIA Flow SDK (BSD-3): Flow's Vulkan backend on an
// application-owned VkDevice. See NvFlowVulkanExternal.h; setup/build_flow.py
// copies this file into source/nvflowext/vulkan before building.

#include "CommonVulkan.h"
#include "NvFlowVulkanExternal.h"

namespace NvFlowVulkan
{

static void logExternalDefault(NvFlowLogLevel level, const char* format, ...)
{
}

static NvFlowDeviceManager* createDeviceManagerExternal(const NvFlowVulkanExternalDesc* desc)
{
    if (!desc || !desc->instance || !desc->physicalDevice || !desc->getInstanceProcAddr)
    {
        return nullptr;
    }

    auto ptr = new DeviceManager();
    ptr->external = NV_FLOW_TRUE;
    ptr->vulkan_module = nullptr;

    NvFlowVulkanLoader_global(&ptr->loader, (PFN_vkGetInstanceProcAddr)desc->getInstanceProcAddr);

    ptr->vulkanInstance = (VkInstance)desc->instance;
    NvFlowVulkanLoader_instance(&ptr->loader, ptr->vulkanInstance);

    // One physical device, the application's. The instance extensions the
    // reference manager looks for are left disabled: they only serve device
    // UUID/LUID reporting for interop, which the shared device does not need.
    VkPhysicalDeviceProperties props = {};
    ptr->loader.vkGetPhysicalDeviceProperties((VkPhysicalDevice)desc->physicalDevice, &props);

    NvFlowPhysicalDeviceDesc physicalDeviceDesc = {};
    ptr->physicalDevices.pushBack((VkPhysicalDevice)desc->physicalDevice);
    ptr->deviceProps.pushBack(props);
    ptr->physicalDeviceDescs.pushBack(physicalDeviceDesc);

    return cast(ptr);
}

static NvFlowDevice* createDeviceExternal(NvFlowDeviceManager* deviceManagerIn, const NvFlowVulkanExternalDesc* desc)
{
    auto deviceManager = cast(deviceManagerIn);
    if (!deviceManager || !desc || !desc->device || !desc->queue || deviceManager->physicalDevices.size == 0u)
    {
        return nullptr;
    }

    auto ptr = new Device();
    ptr->external = NV_FLOW_TRUE;

    ptr->desc.deviceIndex = 0u;
    ptr->desc.enableExternalUsage = NV_FLOW_FALSE;
    ptr->desc.logPrint = desc->logPrint;
    ptr->logPrint = desc->logPrint ? desc->logPrint : logExternalDefault;

    ptr->deviceManager = deviceManager;
    ptr->formatConverter = formatConverter_create();

    ptr->physicalDevice = deviceManager->physicalDevices[0u];
    ptr->graphicsQueueIdx = desc->queueFamilyIndex;

    // No device extensions are claimed: swapchain and external memory /
    // semaphores are what the reference device enables them for, and Flow
    // checks these flags before using either.
    if (desc->shaderStorageImageWriteWithoutFormat)
    {
        ptr->enabledFeatures.shaderStorageImageWriteWithoutFormat = NV_FLOW_TRUE;
    }

    ptr->vulkanDevice = (VkDevice)desc->device;
    NvFlowVulkanLoader_device(&ptr->loader, ptr->vulkanDevice, deviceManager->loader.vkGetDeviceProcAddr);

    deviceManager->loader.vkGetPhysicalDeviceProperties(ptr->physicalDevice, &ptr->physicalDeviceProperties);
    deviceManager->loader.vkGetPhysicalDeviceMemoryProperties(ptr->physicalDevice, &ptr->memoryProperties);

    ptr->graphicsQueue = (VkQueue)desc->queue;

    ptr->deviceQueue = deviceQueue_create(ptr);

    return cast(ptr);
}

static void getTextureExternal(NvFlowTexture* texture, NvFlowVulkanExternalTexture* out)
{
    if (!out)
    {
        return;
    }
    out->image = nullptr;
    out->imageView = nullptr;
    out->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (texture)
    {
        auto ptr = cast(texture);
        out->image = ptr->imageVk;
        out->imageView = ptr->imageViewVk_all;
        out->layout = ptr->restoreBarrier.newLayout;
    }
}

static void getBufferExternal(NvFlowBuffer* buffer, NvFlowVulkanExternalBuffer* out)
{
    if (!out)
    {
        return;
    }
    out->buffer = nullptr;
    out->sizeInBytes = 0llu;
    if (buffer)
    {
        auto ptr = cast(buffer);
        out->buffer = ptr->bufferVk;
        out->sizeInBytes = ptr->desc.sizeInBytes;
    }
}

} // end namespace

NV_FLOW_API NvFlowDeviceManager* NvFlowVulkanExternalCreateDeviceManager(const NvFlowVulkanExternalDesc* desc)
{
    return NvFlowVulkan::createDeviceManagerExternal(desc);
}

NV_FLOW_API NvFlowDevice* NvFlowVulkanExternalCreateDevice(NvFlowDeviceManager* manager, const NvFlowVulkanExternalDesc* desc)
{
    return NvFlowVulkan::createDeviceExternal(manager, desc);
}

NV_FLOW_API void NvFlowVulkanExternalGetTexture(NvFlowTexture* texture, NvFlowVulkanExternalTexture* out)
{
    NvFlowVulkan::getTextureExternal(texture, out);
}

NV_FLOW_API void NvFlowVulkanExternalGetBuffer(NvFlowBuffer* buffer, NvFlowVulkanExternalBuffer* out)
{
    NvFlowVulkan::getBufferExternal(buffer, out);
}
