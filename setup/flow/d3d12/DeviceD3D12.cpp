// Q2RTX addition to the NVIDIA Flow SDK (BSD-3): the device, queue and
// interfaces of Flow's D3D12 backend - vulkan/DeviceVulkan.cpp,
// NvFlowDeviceVulkan.cpp and ExternalVulkan.cpp. There is no reference
// device here: Flow only ever runs on the application's (NvFlowD3D12External.h).

#include "DeviceD3D12.h"
#include "NvFlowD3D12External.h"

namespace NvFlowD3D12
{

NvFlowContextInterface* getContextInterface_d3d12();

static void logExternalDefault(NvFlowLogLevel level, const char* format, ...)
{
}

/// ************************** DeviceManager **************************************

NvFlowDeviceManager* createDeviceManager(NvFlowBool32 enableValidationOnDebugBuild, NvFlowThreadPoolInterface* threadPoolInterface, NvFlowUint threadCount)
{
    // only NvFlowD3D12ExternalCreateDeviceManager
    return nullptr;
}

void destroyDeviceManager(NvFlowDeviceManager* deviceManager)
{
    auto ptr = cast(deviceManager);
    delete ptr;
}

NvFlowBool32 enumerateDevices(NvFlowDeviceManager* manager, NvFlowUint deviceIndex, NvFlowPhysicalDeviceDesc* pDesc)
{
    auto ptr = cast(manager);
    if (deviceIndex < ptr->physicalDeviceDescs.size)
    {
        *pDesc = ptr->physicalDeviceDescs[deviceIndex];
        return NV_FLOW_TRUE;
    }
    return NV_FLOW_FALSE;
}

static NvFlowDeviceManager* createDeviceManagerExternal(const NvFlowD3D12ExternalDesc* desc)
{
    if (!desc || !desc->device)
    {
        return nullptr;
    }

    auto ptr = new DeviceManager();
    ptr->device = (ID3D12Device*)desc->device;

    NvFlowPhysicalDeviceDesc physicalDeviceDesc = {};
    LUID luid = ptr->device->GetAdapterLuid();
    static_assert(sizeof(luid) == sizeof(physicalDeviceDesc.deviceLUID), "LUID size");
    memcpy(physicalDeviceDesc.deviceLUID, &luid, sizeof(luid));
    physicalDeviceDesc.deviceLUIDValid = NV_FLOW_TRUE;
    physicalDeviceDesc.deviceNodeMask = 1u;
    ptr->physicalDeviceDescs.pushBack(physicalDeviceDesc);

    return cast(ptr);
}

/// ************************** Device **************************************

NvFlowDevice* createDevice(NvFlowDeviceManager* deviceManagerIn, const NvFlowDeviceDesc* desc)
{
    // only NvFlowD3D12ExternalCreateDevice
    return nullptr;
}

static NvFlowDevice* createDeviceExternal(NvFlowDeviceManager* deviceManagerIn, const NvFlowD3D12ExternalDesc* desc)
{
    auto deviceManager = cast(deviceManagerIn);
    if (!deviceManager || !desc || !desc->device || !desc->queue)
    {
        return nullptr;
    }

    auto ptr = new Device();

    ptr->desc.deviceIndex = 0u;
    ptr->desc.enableExternalUsage = NV_FLOW_FALSE;
    ptr->desc.logPrint = desc->logPrint;
    ptr->logPrint = desc->logPrint ? desc->logPrint : logExternalDefault;

    ptr->deviceManager = deviceManager;

    ptr->device = (ID3D12Device*)desc->device;
    ptr->device->AddRef();
    ptr->queue = (ID3D12CommandQueue*)desc->queue;
    ptr->queue->AddRef();
    ptr->queueType = ptr->queue->GetDesc().Type;

    ptr->viewDescriptorSize = ptr->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ptr->samplerDescriptorSize = ptr->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // d3d12.dll is loaded: the application made the device with it
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    if (d3d12)
    {
        ptr->serializeRootSignature = (PFN_SerializeRootSignature)GetProcAddress(d3d12, "D3D12SerializeRootSignature");
    }
    if (!ptr->serializeRootSignature)
    {
        ptr->logPrint(eNvFlowLogLevel_error, "D3D12: D3D12SerializeRootSignature not found");
    }

    ptr->deviceQueue = deviceQueue_create(ptr);
    if (!ptr->deviceQueue)
    {
        ptr->queue->Release();
        ptr->device->Release();
        delete ptr;
        return nullptr;
    }

    return cast(ptr);
}

void destroyDevice(NvFlowDeviceManager* manager, NvFlowDevice* device)
{
    auto ptr = cast(device);
    if (!ptr)
    {
        return;
    }

    deviceQueue_destroy(ptr->deviceQueue);

    ptr->queue->Release();
    ptr->device->Release();

    delete ptr;
}

NvFlowDeviceQueue* getDeviceQueue(NvFlowDevice* device)
{
    auto ptr = cast(device);
    return cast(ptr->deviceQueue);
}

void getMemoryStats(NvFlowDevice* device, NvFlowDeviceMemoryStats* dstStats)
{
    auto ptr = cast(device);
    if (dstStats)
    {
        *dstStats = ptr->memoryStats;
    }
}

void device_reportMemoryAllocate(Device* device, NvFlowMemoryType type, NvFlowUint64 bytes)
{
    if (type == eNvFlowMemoryType_device)
    {
        device->memoryStats.deviceMemoryBytes += bytes;
    }
    else if (type == eNvFlowMemoryType_upload)
    {
        device->memoryStats.uploadMemoryBytes += bytes;
    }
    else if (type == eNvFlowMemoryType_readback)
    {
        device->memoryStats.readbackMemoryBytes += bytes;
    }
}

void device_reportMemoryFree(Device* device, NvFlowMemoryType type, NvFlowUint64 bytes)
{
    if (type == eNvFlowMemoryType_device)
    {
        device->memoryStats.deviceMemoryBytes -= bytes;
    }
    else if (type == eNvFlowMemoryType_upload)
    {
        device->memoryStats.uploadMemoryBytes -= bytes;
    }
    else if (type == eNvFlowMemoryType_readback)
    {
        device->memoryStats.readbackMemoryBytes -= bytes;
    }
}

/// ************************** DeviceSemaphore **************************************

NvFlowDeviceSemaphore* createSemaphore(NvFlowDevice* device)
{
    return nullptr;
}

void destroySemaphore(NvFlowDeviceSemaphore* semaphore)
{
}

void getSemaphoreExternalHandle(NvFlowDeviceSemaphore* semaphore, void* dstHandle, NvFlowUint64 dstHandleSize)
{
    memset(dstHandle, 0, dstHandleSize);
}

void closeSemaphoreExternalHandle(NvFlowDeviceSemaphore* semaphore, const void* srcHandle, NvFlowUint64 srcHandleSize)
{
}

/// ************************** DeviceQueue **************************************

DeviceQueue* deviceQueue_create(Device* device)
{
    auto ptr = new DeviceQueue();

    ptr->device = device;

    ID3D12Device* d3d = device->device;
    HRESULT hr = S_OK;
    for (NvFlowUint i = 0u; i < kMaxFramesInFlight && SUCCEEDED(hr); i++)
    {
        hr = d3d->CreateCommandAllocator(device->queueType, IID_PPV_ARGS(&ptr->allocators[i]));
        if (SUCCEEDED(hr))
        {
            hr = d3d->CreateCommandList(0u, device->queueType, ptr->allocators[i], nullptr, IID_PPV_ARGS(&ptr->commandLists[i]));
        }
        if (SUCCEEDED(hr))
        {
            ptr->commandLists[i]->SetName(L"NVIDIA Flow");
            ptr->commandLists[i]->Close();
        }
    }
    if (SUCCEEDED(hr))
    {
        hr = d3d->CreateFence(ptr->lastFenceCompleted, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ptr->fence));
    }
    if (SUCCEEDED(hr))
    {
        hr = d3d->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ptr->idleFence));
    }
    if (SUCCEEDED(hr))
    {
        ptr->fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!ptr->fenceEvent)
        {
            hr = E_FAIL;
        }
    }
    if (FAILED(hr))
    {
        device_log(device, eNvFlowLogLevel_error, "Flow command lists", hr);
        for (NvFlowUint i = 0u; i < kMaxFramesInFlight; i++)
        {
            if (ptr->commandLists[i]) ptr->commandLists[i]->Release();
            if (ptr->allocators[i]) ptr->allocators[i]->Release();
        }
        if (ptr->fence) ptr->fence->Release();
        if (ptr->idleFence) ptr->idleFence->Release();
        delete ptr;
        return nullptr;
    }

    // Second step of flush to prime command list
    flushStepB(ptr);

    ptr->context = context_create(ptr);

    return ptr;
}

void deviceQueue_destroy(DeviceQueue* ptr)
{
    // Wait idle, since context destroy will force destroy resources
    waitIdle(cast(ptr));

    context_destroy(ptr->context);

    // the open list is never executed
    if (ptr->commandList)
    {
        ptr->commandList->Close();
    }
    for (NvFlowUint i = 0u; i < kMaxFramesInFlight; i++)
    {
        ptr->commandLists[i]->Release();
        ptr->allocators[i]->Release();
        for (NvFlowUint64 h = 0u; h < ptr->viewHeaps[i].heaps.size; h++)
        {
            ptr->viewHeaps[i].heaps[h]->Release();
        }
        ptr->viewHeaps[i].heaps.size = 0u;
    }
    ptr->fence->Release();
    ptr->idleFence->Release();
    CloseHandle(ptr->fenceEvent);

    ptr->device = nullptr;

    delete ptr;
}

NvFlowBool32 deviceQueue_allocateViews(DeviceQueue* ptr, NvFlowUint count, ID3D12DescriptorHeap** heap,
    D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    if (count == 0u || count > kViewHeapCapacity)
    {
        return NV_FLOW_FALSE;
    }
    ViewHeapRing& ring = ptr->viewHeaps[ptr->commandListIdx];
    if (ring.heapIdx < ring.heaps.size && ring.offset + count > kViewHeapCapacity)
    {
        ring.heapIdx++;
        ring.offset = 0u;
    }
    if (ring.heapIdx >= ring.heaps.size)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kViewHeapCapacity;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap* newHeap = nullptr;
        HRESULT hr = ptr->device->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&newHeap));
        if (FAILED(hr))
        {
            device_log(ptr->device, eNvFlowLogLevel_error, "Flow descriptor heap", hr);
            return NV_FLOW_FALSE;
        }
        newHeap->SetName(L"NVIDIA Flow views");
        ring.heaps.pushBack(newHeap);
        ring.heapIdx = (NvFlowUint)(ring.heaps.size - 1u);
        ring.offset = 0u;
    }

    ID3D12DescriptorHeap* h = ring.heaps[ring.heapIdx];
    *heap = h;
    *cpu = h->GetCPUDescriptorHandleForHeapStart();
    *gpu = h->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += (SIZE_T)ring.offset * ptr->device->viewDescriptorSize;
    gpu->ptr += (UINT64)ring.offset * ptr->device->viewDescriptorSize;
    ring.offset += count;
    return NV_FLOW_TRUE;
}

int flushStepA(DeviceQueue* ptr)
{
    if (ptr->context)
    {
        context_flushNodes(ptr->context);
    }

    HRESULT hr = ptr->commandList->Close();
    if (SUCCEEDED(hr))
    {
        ID3D12CommandList* lists[1] = { ptr->commandList };
        ptr->device->queue->ExecuteCommandLists(1u, lists);
    }
    else
    {
        device_log(ptr->device, eNvFlowLogLevel_error, "Flow command list Close", hr);
    }
    ptr->device->queue->Signal(ptr->fence, ptr->nextFenceValue);

    // mark signaled fence value
    ptr->fenceValues[ptr->commandListIdx] = ptr->nextFenceValue;
    ptr->fenceActive[ptr->commandListIdx] = NV_FLOW_TRUE;

    // increment fence value
    ptr->nextFenceValue++;

    return ptr->device->device->GetDeviceRemovedReason() != S_OK ? 1 : 0;
}

void deviceQueue_fenceUpdate(DeviceQueue* ptr, NvFlowUint fenceIdx, NvFlowBool32 blocking)
{
    if (ptr->fenceActive[fenceIdx])
    {
        NvFlowUint64 value = ptr->fenceValues[fenceIdx];
        NvFlowBool32 done = ptr->fence->GetCompletedValue() >= value;
        if (!done && blocking)
        {
            if (SUCCEEDED(ptr->fence->SetEventOnCompletion(value, ptr->fenceEvent)))
            {
                WaitForSingleObject(ptr->fenceEvent, INFINITE);
            }
            done = NV_FLOW_TRUE;
        }
        if (done)
        {
            ptr->fenceActive[fenceIdx] = NV_FLOW_FALSE;
            if (value > ptr->lastFenceCompleted)
            {
                ptr->lastFenceCompleted = value;
            }
        }
    }
}

void flushStepB(DeviceQueue* ptr)
{
    ptr->commandListIdx = (ptr->commandListIdx + 1) % kMaxFramesInFlight;
    ptr->commandList = ptr->commandLists[ptr->commandListIdx];

    // non-blocking update of fence values
    for (NvFlowUint fenceIdx = 0u; fenceIdx < kMaxFramesInFlight; fenceIdx++)
    {
        deviceQueue_fenceUpdate(ptr, fenceIdx, NV_FLOW_FALSE);
    }

    // blocking update of fence values
    deviceQueue_fenceUpdate(ptr, ptr->commandListIdx, NV_FLOW_TRUE);

    ID3D12CommandAllocator* allocator = ptr->allocators[ptr->commandListIdx];
    allocator->Reset();
    ptr->commandList->Reset(allocator, nullptr);

    // this frame's descriptors are free again
    ptr->viewHeaps[ptr->commandListIdx].heapIdx = 0u;
    ptr->viewHeaps[ptr->commandListIdx].offset = 0u;
    ptr->heapsBound = NV_FLOW_FALSE;
    ptr->boundViewHeap = nullptr;
    ptr->boundRootSignature = nullptr;

    if (ptr->context)
    {
        context_resetNodes(ptr->context);
    }
}

int flush(NvFlowDeviceQueue* deviceQueue, NvFlowUint64* flushedFrameID, NvFlowDeviceSemaphore* waitSemaphore, NvFlowDeviceSemaphore* signalSemaphore)
{
    auto ptr = cast(deviceQueue);

    *flushedFrameID = ptr->nextFenceValue;

    int ret = flushStepA(ptr);
    flushStepB(ptr);

    return ret;
}

void waitIdle(NvFlowDeviceQueue* deviceQueue)
{
    auto ptr = cast(deviceQueue);

    // everything on the queue, the application's work included, as
    // vkQueueWaitIdle would
    ptr->idleFenceValue++;
    if (SUCCEEDED(ptr->device->queue->Signal(ptr->idleFence, ptr->idleFenceValue)) &&
        ptr->idleFence->GetCompletedValue() < ptr->idleFenceValue &&
        SUCCEEDED(ptr->idleFence->SetEventOnCompletion(ptr->idleFenceValue, ptr->fenceEvent)))
    {
        WaitForSingleObject(ptr->fenceEvent, INFINITE);
    }

    for (NvFlowUint fenceIdx = 0u; fenceIdx < kMaxFramesInFlight; fenceIdx++)
    {
        deviceQueue_fenceUpdate(ptr, fenceIdx, NV_FLOW_TRUE);
    }
}

void waitForFrame(NvFlowDeviceQueue* deviceQueue, NvFlowUint64 frameID)
{
    auto ptr = cast(deviceQueue);

    // avoid waiting on future frames
    if (frameID >= ptr->nextFenceValue)
    {
        return;
    }

    while (ptr->lastFenceCompleted < frameID)
    {
        NvFlowUint64 minFenceValue = 0llu;
        NvFlowUint minFenceIdx = 0u;
        for (NvFlowUint fenceIdx = 0u; fenceIdx < kMaxFramesInFlight; fenceIdx++)
        {
            if (ptr->fenceActive[fenceIdx])
            {
                NvFlowUint64 frameFenceValue = ptr->fenceValues[fenceIdx];
                if (minFenceValue == 0 || frameFenceValue < minFenceValue)
                {
                    minFenceValue = frameFenceValue;
                    minFenceIdx = fenceIdx;
                }
            }
        }
        // wait for min frame
        if (minFenceValue > 0u)
        {
            deviceQueue_fenceUpdate(ptr, minFenceIdx, NV_FLOW_TRUE);
        }
        else
        {
            break;
        }
    }
}

NvFlowUint64 getLastFrameCompleted(NvFlowDeviceQueue* queue)
{
    auto ptr = cast(queue);
    return ptr->lastFenceCompleted;
}

NvFlowContextInterface* getContextInterface(NvFlowDeviceQueue* ptr)
{
    return getContextInterface_d3d12();
}

NvFlowContext* getContext(NvFlowDeviceQueue* queue)
{
    auto deviceQueue = cast(queue);

    return cast(deviceQueue->context);
}

/// ************************** Swapchain **************************************

NvFlowSwapchain* createSwapchain(NvFlowDeviceQueue* queue, const NvFlowSwapchainDesc* desc)
{
    return nullptr;
}

void destroySwapchain(NvFlowSwapchain* swapchain)
{
}

void resizeSwapchain(NvFlowSwapchain* swapchain, NvFlowUint width, NvFlowUint height)
{
}

int presentSwapchain(NvFlowSwapchain* swapchain, NvFlowBool32 vsync, NvFlowUint64* flushedFrameID)
{
    if (flushedFrameID)
    {
        *flushedFrameID = 0u;
    }
    return 1;
}

NvFlowTexture* getSwapchainFrontTexture(NvFlowSwapchain* swapchain)
{
    return nullptr;
}

/// ************************** Context interface **************************************

static void getContextConfig(NvFlowContext* context, NvFlowContextConfig* config)
{
    config->api = eNvFlowContextApi_d3d12;
    config->textureBinding = eNvFlowTextureBindingType_separateSampler;
}

static NvFlowBool32 isFeatureSupported(NvFlowContext* context, NvFlowContextFeature feature)
{
    // aliasResourceFormats would need typeless resources; bufferExternalHandle
    // would need shared heaps. Flow does without either.
    return NV_FLOW_FALSE;
}

static NvFlowUint64 getCurrentFrame(NvFlowContext* context)
{
    Context* ctx = cast(context);
    return ctx->deviceQueue->nextFenceValue;
}

static NvFlowUint64 getLastFrameCompleted_context(NvFlowContext* context)
{
    Context* ctx = cast(context);
    return ctx->deviceQueue->lastFenceCompleted;
}

static NvFlowLogPrint_t getLogPrint(NvFlowContext* context)
{
    Context* ctx = cast(context);
    return ctx->logPrint;
}

static void executeTasks(NvFlowContext* context, NvFlowUint taskCount, NvFlowUint taskGranularity, NvFlowContextThreadPoolTask_t task, void* userdata)
{
    Context* ctx = cast(context);
    ctx->threadPoolInterface.execute(ctx->threadPool, taskCount, taskGranularity, task, userdata);
}

NvFlowContextInterface* getContextInterface_d3d12()
{
    static NvFlowContextInterface iface = { NV_FLOW_REFLECT_INTERFACE_INIT(NvFlowContextInterface) };

    iface.getContextConfig = getContextConfig;
    iface.isFeatureSupported = isFeatureSupported;
    iface.getCurrentFrame = getCurrentFrame;
    iface.getLastFrameCompleted = getLastFrameCompleted_context;
    iface.getCurrentGlobalFrame = getCurrentFrame;
    iface.getLastGlobalFrameCompleted = getLastFrameCompleted_context;
    iface.getLogPrint = getLogPrint;

    iface.executeTasks = executeTasks;

    iface.createBuffer = createBuffer;
    iface.destroyBuffer = destroyBuffer;
    iface.getBufferTransient = getBufferTransient;
    iface.registerBufferAsTransient = registerBufferAsTransient;
    iface.aliasBufferTransient = aliasBufferTransient;
    iface.enqueueAcquireBuffer = enqueueAcquireBuffer;
    iface.getAcquiredBuffer = getAcquiredBuffer;
    iface.mapBuffer = mapBuffer;
    iface.unmapBuffer = unmapBuffer;
    iface.getBufferTransientById = getBufferTransientById;
    iface.getBufferExternalHandle = getBufferExternalHandle;
    iface.closeBufferExternalHandle = closeBufferExternalHandle;
    iface.createBufferFromExternalHandle = createBufferFromExternalHandle;

    iface.createTexture = createTexture;
    iface.destroyTexture = destroyTexture;
    iface.getTextureTransient = getTextureTransient;
    iface.registerTextureAsTransient = registerTextureAsTransient;
    iface.aliasTextureTransient = aliasTextureTransient;
    iface.enqueueAcquireTexture = enqueueAcquireTexture;
    iface.getAcquiredTexture = getAcquiredTexture;
    iface.getTextureTransientById = getTextureTransientById;

    iface.createSampler = createSampler;
    iface.getDefaultSampler = getDefaultSampler;
    iface.destroySampler = destroySampler;

    iface.createComputePipeline = createComputePipeline;
    iface.destroyComputePipeline = destroyComputePipeline;

    iface.addPassCompute = addPassCompute;
    iface.addPassCopyBuffer = addPassCopyBuffer;
    iface.addPassCopyBufferToTexture = addPassCopyBufferToTexture;
    iface.addPassCopyTextureToBuffer = addPassCopyTextureToBuffer;
    iface.addPassCopyTexture = addPassCopyTexture;

    return &iface;
}

/// ************************** External **************************************

static void getTextureExternal(NvFlowTexture* texture, NvFlowD3D12ExternalTexture* out)
{
    if (!out)
    {
        return;
    }
    out->resource = nullptr;
    out->format = 0u;
    out->state = 0u;
    if (texture)
    {
        auto ptr = cast(texture);
        out->resource = ptr->resource;
        out->format = (NvFlowUint)ptr->format;
        out->state = (NvFlowUint)ptr->restoreState;
    }
}

static void getBufferExternal(NvFlowBuffer* buffer, NvFlowD3D12ExternalBuffer* out)
{
    if (!out)
    {
        return;
    }
    out->resource = nullptr;
    out->sizeInBytes = 0llu;
    out->structureStride = 0u;
    out->format = 0u;
    out->state = 0u;
    if (buffer)
    {
        auto ptr = cast(buffer);
        out->resource = ptr->resource;
        out->sizeInBytes = ptr->desc.sizeInBytes;
        out->structureStride = ptr->desc.structureStride;
        out->format = (NvFlowUint)format_toDxgi(ptr->desc.format);
        out->state = (NvFlowUint)ptr->restoreState;
    }
}

} // end namespace

NvFlowDeviceInterface* NvFlowGetDeviceInterface_d3d12()
{
    using namespace NvFlowD3D12;
    static NvFlowDeviceInterface iface = { NV_FLOW_REFLECT_INTERFACE_INIT(NvFlowDeviceInterface) };

    iface.createDeviceManager = createDeviceManager;
    iface.destroyDeviceManager = destroyDeviceManager;
    iface.enumerateDevices = enumerateDevices;

    iface.createDevice = createDevice;
    iface.destroyDevice = destroyDevice;

    iface.createSemaphore = createSemaphore;
    iface.destroySemaphore = destroySemaphore;
    iface.getSemaphoreExternalHandle = getSemaphoreExternalHandle;
    iface.closeSemaphoreExternalHandle = closeSemaphoreExternalHandle;

    iface.getDeviceQueue = getDeviceQueue;
    iface.flush = flush;
    iface.getLastFrameCompleted = getLastFrameCompleted;
    iface.waitForFrame = waitForFrame;
    iface.waitIdle = waitIdle;
    iface.getContextInterface = getContextInterface;
    iface.getContext = getContext;

    iface.createSwapchain = createSwapchain;
    iface.destroySwapchain = destroySwapchain;
    iface.resizeSwapchain = resizeSwapchain;
    iface.presentSwapchain = presentSwapchain;
    iface.getSwapchainFrontTexture = getSwapchainFrontTexture;

    iface.enableProfiler = enableProfiler;
    iface.disableProfiler = disableProfiler;

    iface.registerBufferId = registerBufferId;
    iface.registerTextureId = registerTextureId;
    iface.unregisterBufferId = unregisterBufferId;
    iface.unregisterTextureId = unregisterTextureId;
    iface.setResourceMinLifetime = setResourceMinLifetime;

    iface.getBufferExternalHandle = device_getBufferExternalHandle;
    iface.closeBufferExternalHandle = device_closeBufferExternalHandle;

    iface.getMemoryStats = getMemoryStats;

    return &iface;
}

NV_FLOW_API NvFlowDeviceManager* NvFlowD3D12ExternalCreateDeviceManager(const NvFlowD3D12ExternalDesc* desc)
{
    return NvFlowD3D12::createDeviceManagerExternal(desc);
}

NV_FLOW_API NvFlowDevice* NvFlowD3D12ExternalCreateDevice(NvFlowDeviceManager* manager, const NvFlowD3D12ExternalDesc* desc)
{
    return NvFlowD3D12::createDeviceExternal(manager, desc);
}

NV_FLOW_API void NvFlowD3D12ExternalGetTexture(NvFlowTexture* texture, NvFlowD3D12ExternalTexture* out)
{
    NvFlowD3D12::getTextureExternal(texture, out);
}

NV_FLOW_API void NvFlowD3D12ExternalGetBuffer(NvFlowBuffer* buffer, NvFlowD3D12ExternalBuffer* out)
{
    NvFlowD3D12::getBufferExternal(buffer, out);
}
