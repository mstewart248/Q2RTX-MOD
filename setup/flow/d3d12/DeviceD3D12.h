// Q2RTX addition to the NVIDIA Flow SDK (BSD-3): a Direct3D 12 backend for
// Flow, on an application-owned ID3D12Device and queue. See
// NvFlowD3D12External.h. It follows source/nvflowext/vulkan/ closely - the
// same objects, the same frame graph, the same pooling and lifetimes - with
// D3D12's states, descriptor heaps and root signatures in place of Vulkan's
// barriers, descriptor sets and pipeline layouts. setup/build_flow.py copies
// it into source/nvflowext/d3d12.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <string.h>

#include "NvFlowExt.h"
#include "NvFlowArray.h"

namespace NvFlowD3D12
{
    static const NvFlowUint kMaxFramesInFlight = 3u;

    // shader-visible CBV/SRV/UAV descriptors per heap; a frame takes more
    // heaps when it runs out
    static const NvFlowUint kViewHeapCapacity = 16384u;
    // one shader-visible sampler heap, holding every distinct sampler table
    static const NvFlowUint kSamplerHeapCapacity = 2048u;

    struct DeviceManager;
    struct Device;
    struct DeviceSemaphore;
    struct DeviceQueue;
    struct Swapchain;
    struct Context;

    struct Buffer;
    struct BufferTransient;
    struct BufferAcquire;
    struct Texture;
    struct TextureTransient;
    struct TextureAcquire;
    struct Sampler;

    struct ComputePipeline;

    NV_FLOW_CAST_PAIR(NvFlowDeviceManager, DeviceManager)
    NV_FLOW_CAST_PAIR(NvFlowDevice, Device)
    NV_FLOW_CAST_PAIR(NvFlowDeviceSemaphore, DeviceSemaphore)
    NV_FLOW_CAST_PAIR(NvFlowDeviceQueue, DeviceQueue)
    NV_FLOW_CAST_PAIR(NvFlowSwapchain, Swapchain)
    NV_FLOW_CAST_PAIR(NvFlowContext, Context)

    NV_FLOW_CAST_PAIR(NvFlowBuffer, Buffer)
    NV_FLOW_CAST_PAIR(NvFlowBufferTransient, BufferTransient)
    NV_FLOW_CAST_PAIR(NvFlowBufferAcquire, BufferAcquire)
    NV_FLOW_CAST_PAIR(NvFlowTexture, Texture)
    NV_FLOW_CAST_PAIR(NvFlowTextureTransient, TextureTransient)
    NV_FLOW_CAST_PAIR(NvFlowTextureAcquire, TextureAcquire)
    NV_FLOW_CAST_PAIR(NvFlowSampler, Sampler)

    NV_FLOW_CAST_PAIR(NvFlowComputePipeline, ComputePipeline)

    typedef HRESULT(WINAPI* PFN_SerializeRootSignature)(const D3D12_ROOT_SIGNATURE_DESC* desc,
        D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** error);

    struct DeviceManager
    {
        ID3D12Device* device = nullptr;     // the application's, not referenced
        NvFlowArray<NvFlowPhysicalDeviceDesc> physicalDeviceDescs;
    };

    NvFlowDeviceManager* createDeviceManager(NvFlowBool32 enableValidationOnDebugBuild, NvFlowThreadPoolInterface* threadPoolInterface, NvFlowUint threadCount);
    void destroyDeviceManager(NvFlowDeviceManager* manager);
    NvFlowBool32 enumerateDevices(NvFlowDeviceManager* manager, NvFlowUint deviceIndex, NvFlowPhysicalDeviceDesc* pDesc);

    struct Device
    {
        NvFlowDeviceDesc desc = {};
        NvFlowLogPrint_t logPrint = nullptr;

        DeviceManager* deviceManager = nullptr;

        ID3D12Device* device = nullptr;     // the application's, AddRef'd
        ID3D12CommandQueue* queue = nullptr;    // the application's, AddRef'd
        D3D12_COMMAND_LIST_TYPE queueType = D3D12_COMMAND_LIST_TYPE_DIRECT;

        PFN_SerializeRootSignature serializeRootSignature = nullptr;
        UINT viewDescriptorSize = 0u;
        UINT samplerDescriptorSize = 0u;

        DeviceQueue* deviceQueue = nullptr;

        NvFlowDeviceMemoryStats memoryStats = {};
    };

    NvFlowDevice* createDevice(NvFlowDeviceManager* deviceManager, const NvFlowDeviceDesc* desc);
    void destroyDevice(NvFlowDeviceManager* deviceManager, NvFlowDevice* device);
    NvFlowDeviceQueue* getDeviceQueue(NvFlowDevice* device);
    void getMemoryStats(NvFlowDevice* device, NvFlowDeviceMemoryStats* dstStats);

    void device_reportMemoryAllocate(Device* device, NvFlowMemoryType type, NvFlowUint64 bytes);
    void device_reportMemoryFree(Device* device, NvFlowMemoryType type, NvFlowUint64 bytes);

    // Cross-API semaphores are not supported: the application shares the queue.
    struct DeviceSemaphore
    {
        Device* device = nullptr;
    };

    NvFlowDeviceSemaphore* createSemaphore(NvFlowDevice* device);
    void destroySemaphore(NvFlowDeviceSemaphore* semaphore);
    void getSemaphoreExternalHandle(NvFlowDeviceSemaphore* semaphore, void* dstHandle, NvFlowUint64 dstHandleSize);
    void closeSemaphoreExternalHandle(NvFlowDeviceSemaphore* semaphore, const void* srcHandle, NvFlowUint64 srcHandleSize);

    // The shader-visible CBV/SRV/UAV descriptors of one frame in flight: heaps
    // filled front to back, reused once that frame's fence has passed.
    struct ViewHeapRing
    {
        NvFlowArray<ID3D12DescriptorHeap*> heaps;
        NvFlowUint heapIdx = 0u;
        NvFlowUint offset = 0u;
    };

    struct DeviceQueue
    {
        Device* device = nullptr;

        ID3D12CommandAllocator* allocators[kMaxFramesInFlight] = { nullptr };
        ID3D12GraphicsCommandList* commandLists[kMaxFramesInFlight] = { nullptr };
        NvFlowUint64 fenceValues[kMaxFramesInFlight] = { 0u };
        NvFlowBool32 fenceActive[kMaxFramesInFlight] = { NV_FLOW_FALSE };
        ViewHeapRing viewHeaps[kMaxFramesInFlight];

        int commandListIdx = 0;
        ID3D12GraphicsCommandList* commandList = nullptr;

        // Flow frame ids: what each flush signals
        ID3D12Fence* fence = nullptr;
        // waitIdle's, signalled after everything on the queue
        ID3D12Fence* idleFence = nullptr;
        NvFlowUint64 idleFenceValue = 0u;
        HANDLE fenceEvent = nullptr;

        NvFlowUint64 lastFenceCompleted = 1u;
        NvFlowUint64 nextFenceValue = 2u;

        // what the open command list has bound
        NvFlowBool32 heapsBound = NV_FLOW_FALSE;
        ID3D12DescriptorHeap* boundViewHeap = nullptr;
        ID3D12RootSignature* boundRootSignature = nullptr;

        Context* context = nullptr;
    };

    int flush(NvFlowDeviceQueue* ptr, NvFlowUint64* flushedFrameID, NvFlowDeviceSemaphore* waitSemaphore, NvFlowDeviceSemaphore* signalSemaphore);
    NvFlowUint64 getLastFrameCompleted(NvFlowDeviceQueue* queue);
    void waitForFrame(NvFlowDeviceQueue* ptr, NvFlowUint64 frameID);
    void waitIdle(NvFlowDeviceQueue* ptr);
    NvFlowContextInterface* getContextInterface(NvFlowDeviceQueue* ptr);
    NvFlowContext* getContext(NvFlowDeviceQueue* ptr);

    DeviceQueue* deviceQueue_create(Device* device);
    void deviceQueue_destroy(DeviceQueue* deviceQueue);
    int flushStepA(DeviceQueue* ptr);
    void flushStepB(DeviceQueue* ptr);
    void deviceQueue_fenceUpdate(DeviceQueue* ptr, NvFlowUint fenceIdx, NvFlowBool32 blocking);

    // Shader-visible descriptors for one dispatch, in the open list's frame,
    // and the heap they are in (a new one when the frame's current one is full).
    NvFlowBool32 deviceQueue_allocateViews(DeviceQueue* ptr, NvFlowUint count, ID3D12DescriptorHeap** heap,
        D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);

    // No D3D12 swapchain: Flow's own window is the editor's, never the game's.
    NvFlowSwapchain* createSwapchain(NvFlowDeviceQueue* queue, const NvFlowSwapchainDesc* desc);
    void destroySwapchain(NvFlowSwapchain* swapchain);
    void resizeSwapchain(NvFlowSwapchain* swapchain, NvFlowUint width, NvFlowUint height);
    int presentSwapchain(NvFlowSwapchain* swapchain, NvFlowBool32 vsync, NvFlowUint64* flushedFrameID);
    NvFlowTexture* getSwapchainFrontTexture(NvFlowSwapchain* swapchain);

    struct Buffer
    {
        int refCount = 0;
        NvFlowUint64 lastActive = NV_FLOW_FALSE;
        NvFlowBufferDesc desc = {};
        NvFlowMemoryType memoryType = eNvFlowMemoryType_device;
        NvFlowUint64 allocationBytes = 0llu;

        ID3D12Resource* resource = nullptr;
        NvFlowUint64 resourceBytes = 0llu;      // desc.sizeInBytes rounded up for CBVs
        void* mappedData = nullptr;

        // Upload and readback buffers never change state (GENERIC_READ,
        // COPY_DEST); device buffers start each Flow frame in COMMON.
        D3D12_RESOURCE_STATES restoreState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
        NvFlowBool32 fixedState = NV_FLOW_FALSE;
    };

    struct BufferTransient
    {
        NvFlowBufferDesc desc = {};
        Buffer* buffer = nullptr;
        BufferTransient* aliasBuffer = nullptr;
        NvFlowFormat aliasFormat = eNvFlowFormat_unknown;
        NvFlowUint aliasStructureStride = ~0u;
        int nodeBegin = 0;
        int nodeEnd = 0;
    };

    struct BufferAcquire
    {
        BufferTransient* bufferTransient = nullptr;
        Buffer* buffer = nullptr;
    };

    NvFlowBuffer* createBuffer(NvFlowContext* context, NvFlowMemoryType memoryType, const NvFlowBufferDesc* desc);
    void destroyBuffer(NvFlowContext* context, NvFlowBuffer* buffer);
    NvFlowBufferTransient* getBufferTransient(NvFlowContext* context, const NvFlowBufferDesc* desc);
    NvFlowBufferTransient* registerBufferAsTransient(NvFlowContext* context, NvFlowBuffer* buffer);
    NvFlowBufferTransient* aliasBufferTransient(NvFlowContext* context, NvFlowBufferTransient* buffer, NvFlowFormat format, NvFlowUint structureStride);
    NvFlowBufferAcquire* enqueueAcquireBuffer(NvFlowContext* context, NvFlowBufferTransient* buffer);
    NvFlowBool32 getAcquiredBuffer(NvFlowContext* context, NvFlowBufferAcquire* acquire, NvFlowBuffer** outBuffer);
    void* mapBuffer(NvFlowContext* context, NvFlowBuffer* buffer);
    void unmapBuffer(NvFlowContext* context, NvFlowBuffer* buffer);
    NvFlowBufferTransient* getBufferTransientById(NvFlowContext* context, NvFlowUint64 bufferId);
    void getBufferExternalHandle(NvFlowContext* context, NvFlowBuffer* buffer, NvFlowInteropHandle* dstHandle);
    void closeBufferExternalHandle(NvFlowContext* context, NvFlowBuffer* buffer, const NvFlowInteropHandle* srcHandle);
    NvFlowBuffer* createBufferFromExternalHandle(NvFlowContext* context, const NvFlowBufferDesc* desc, const NvFlowInteropHandle* interopHandle);
    void device_getBufferExternalHandle(NvFlowContext* context, NvFlowBuffer* buffer, void* dstHandle, NvFlowUint64 dstHandleSize, NvFlowUint64* pBufferSizeInBytes);
    void device_closeBufferExternalHandle(NvFlowContext* context, NvFlowBuffer* buffer, const void* srcHandle, NvFlowUint64 srcHandleSize);

    Buffer* buffer_create(Context* context, NvFlowMemoryType memoryType, const NvFlowBufferDesc* desc);
    void buffer_destroy(Context* context, Buffer* buffer);
    void context_destroyBuffers(Context* context);
    // The view a descriptor write of `type` wants, into `dst`.
    void buffer_writeView(Context* context, BufferTransient* transient, NvFlowDescriptorType type, D3D12_CPU_DESCRIPTOR_HANDLE dst);

    struct Texture
    {
        int refCount = 0;
        NvFlowUint64 lastActive = NV_FLOW_FALSE;
        NvFlowTextureDesc desc = {};
        NvFlowUint64 allocationBytes = 0llu;

        ID3D12Resource* resource = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

        D3D12_RESOURCE_STATES restoreState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    };

    struct TextureTransient
    {
        NvFlowTextureDesc desc = {};
        Texture* texture = nullptr;
        TextureTransient* aliasTexture = nullptr;
        NvFlowFormat aliasFormat = eNvFlowFormat_unknown;
        int nodeBegin = 0;
        int nodeEnd = 0;
    };

    struct TextureAcquire
    {
        TextureTransient* textureTransient = nullptr;
        Texture* texture = nullptr;
    };

    NvFlowTexture* createTexture(NvFlowContext* context, const NvFlowTextureDesc* desc);
    void destroyTexture(NvFlowContext* context, NvFlowTexture* texture);
    NvFlowTextureTransient* getTextureTransient(NvFlowContext* context, const NvFlowTextureDesc* desc);
    NvFlowTextureTransient* registerTextureAsTransient(NvFlowContext* context, NvFlowTexture* texture);
    NvFlowTextureTransient* aliasTextureTransient(NvFlowContext* context, NvFlowTextureTransient* texture, NvFlowFormat format);
    NvFlowTextureAcquire* enqueueAcquireTexture(NvFlowContext* context, NvFlowTextureTransient* texture);
    NvFlowBool32 getAcquiredTexture(NvFlowContext* context, NvFlowTextureAcquire* acquire, NvFlowTexture** outTexture);
    NvFlowTextureTransient* getTextureTransientById(NvFlowContext* context, NvFlowUint64 textureId);

    Texture* texture_create(Context* context, const NvFlowTextureDesc* desc);
    void texture_destroy(Context* context, Texture* texture);
    void context_destroyTextures(Context* context);
    void texture_writeView(Context* context, TextureTransient* transient, NvFlowDescriptorType type, D3D12_CPU_DESCRIPTOR_HANDLE dst);

    struct Sampler
    {
        NvFlowBool32 isActive = NV_FLOW_FALSE;
        NvFlowUint64 lastActive = NV_FLOW_FALSE;
        D3D12_SAMPLER_DESC desc = {};
    };

    NvFlowSampler* createSampler(NvFlowContext* context, const NvFlowSamplerDesc* desc);
    NvFlowSampler* getDefaultSampler(NvFlowContext* context);
    void destroySampler(NvFlowContext* context, NvFlowSampler* sampler);

    Sampler* sampler_create(Context* context, const NvFlowSamplerDesc* desc);
    void sampler_destroy(Context* context, Sampler* sampler);
    void context_destroySamplers(Context* context);

    // A dispatch's samplers, s0 onward, made once into the shader-visible
    // sampler heap and kept: samplers are never freed before the context.
    static const NvFlowUint kMaxSamplerTable = 16u;
    struct SamplerTable
    {
        NvFlowUint count = 0u;
        Sampler* samplers[kMaxSamplerTable] = { nullptr };
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
    };

    struct ComputePipeline
    {
        NvFlowComputePipelineDesc desc = {};

        // descriptor table 0: b0.., then t0.., then u0..; table 1: s0..
        NvFlowUint numCbv = 0u;
        NvFlowUint numSrv = 0u;
        NvFlowUint numUav = 0u;
        NvFlowUint numSampler = 0u;

        ID3D12RootSignature* rootSignature = nullptr;
        ID3D12PipelineState* pipeline = nullptr;
    };

    NvFlowComputePipeline* createComputePipeline(NvFlowContext* context, const NvFlowComputePipelineDesc* desc);
    void destroyComputePipeline(NvFlowContext* context, NvFlowComputePipeline* pipeline);

    void computePipeline_dispatch(Context* context, const NvFlowPassComputeParams* params);

    struct ProfilerEntry
    {
        const char* label;
        NvFlowUint64 cpuValue;
        NvFlowUint64 gpuValue;
    };

    struct ProfilerCapture
    {
        ID3D12QueryHeap* queryHeap = nullptr;
        ID3D12Resource* queryBuffer = nullptr;
        NvFlowUint64* queryMapped = nullptr;
        NvFlowUint64 queryFrequency = 0u;
        NvFlowUint64 queryReadbackFenceVal = ~0llu;

        NvFlowUint state = 0u;
        NvFlowUint64 captureID = 0llu;
        NvFlowUint64 cpuFreq = 0llu;
        NvFlowUint64 capacity = 0u;

        NvFlowArray<ProfilerEntry> entries;
        NvFlowArray<NvFlowProfilerEntry> deltaEntries;
    };

    void profilerCapture_init(Context* context, ProfilerCapture* ptr, NvFlowUint64 capacity);
    void profilerCapture_destroy(Context* context, ProfilerCapture* ptr);
    void profilerCapture_reset(Context* context, ProfilerCapture* ptr, NvFlowUint64 minCapacity, NvFlowUint64 captureID);
    void profilerCapture_timestamp(Context* context, ProfilerCapture* ptr, const char* label);
    void profilerCapture_download(Context* context, ProfilerCapture* ptr);
    NvFlowBool32 profilerCapture_mapResults(Context* context, ProfilerCapture* ptr, NvFlowUint64* pNumEntries, NvFlowProfilerEntry** pEntries);
    void profilerCapture_unmapResults(Context* context, ProfilerCapture* ptr);

    struct Profiler
    {
        NvFlowArray<ProfilerCapture> captures;
        NvFlowUint64 currentCaptureIndex = 0u;
        NvFlowUint64 currentCaptureID = 0llu;

        void* userdata = nullptr;
        void(NV_FLOW_ABI* reportEntries)(void* userdata, NvFlowUint64 captureID, NvFlowUint numEntries, NvFlowProfilerEntry* entries) = nullptr;
    };

    Profiler* profiler_create(Context* context);
    void profiler_destroy(Context* context, Profiler* ptr);
    void profiler_beginCapture(Context* context, Profiler* ptr, NvFlowUint64 numEntries);
    void profiler_endCapture(Context* context, Profiler* ptr);
    void profiler_processCaptures(Context* context, Profiler* ptr);
    void profiler_timestamp(Context* context, Profiler* ptr, const char* label);

    void enableProfiler(NvFlowContext* context, void* userdata, void(NV_FLOW_ABI* reportEntries)(void* userdata, NvFlowUint64 captureID, NvFlowUint numEntries, NvFlowProfilerEntry* entries));
    void disableProfiler(NvFlowContext* context);

    enum ContextNodeType
    {
        eContextNodeType_unknown = 0,
        eContextNodeType_compute = 1,
        eContextNodeType_copyBuffer = 2,
        eContextNodeType_copyBufferToTexture = 3,
        eContextNodeType_copyTextureToBuffer = 4,
        eContextNodeType_copyTexture = 5,

        eContextNodeType_maxEnum = 0x7FFFFFFF
    };

    struct ContextNodeMemoryParams
    {
        unsigned char data[128u];
    };

    union ContextNodeParams
    {
        NvFlowPassComputeParams compute;
        NvFlowPassCopyBufferParams copyBuffer;
        NvFlowPassCopyBufferToTextureParams copyBufferToTexture;
        NvFlowPassCopyTextureToBufferParams copyTextureToBuffer;
        NvFlowPassCopyTextureParams copyTexture;
        ContextNodeMemoryParams memory;
    };

    struct ContextNode
    {
        ContextNodeType type = eContextNodeType_unknown;
        ContextNodeParams params = {};
        const char* label = "Unknown";
        NvFlowArray<NvFlowDescriptorWrite> descriptorWrites;
        NvFlowArray<NvFlowResource> resources;

        NvFlowArray<BufferTransient*> bufferTransientsCreate;
        NvFlowArray<TextureTransient*> textureTransientsCreate;
        NvFlowArray<BufferTransient*> bufferTransientsDestroy;
        NvFlowArray<TextureTransient*> textureTransientsDestroy;
        NvFlowArray<D3D12_RESOURCE_BARRIER> barriers;
    };

    struct RegisteredResource
    {
        NvFlowBuffer* buffer;
        NvFlowTexture* texture;
        NvFlowUint64 uid;
    };

    struct Context
    {
        DeviceQueue* deviceQueue = nullptr;

        NvFlowArrayPointer<Buffer*> pool_buffers;
        NvFlowArrayPointer<Texture*> pool_textures;
        NvFlowArrayPointer<Sampler*> pool_samplers;

        NvFlowArrayPointer<BufferTransient*> bufferTransients;
        NvFlowArrayPointer<TextureTransient*> textureTransients;

        NvFlowArrayPointer<BufferAcquire*> bufferAcquires;
        NvFlowArrayPointer<TextureAcquire*> textureAcquires;

        NvFlowArray<ContextNode> nodes;

        NvFlowArray<D3D12_RESOURCE_BARRIER> restore_barriers;

        ID3D12DescriptorHeap* samplerHeap = nullptr;
        NvFlowUint samplerHeapCount = 0u;
        NvFlowArray<SamplerTable> samplerTables;

        Profiler* profiler = nullptr;

        NvFlowUint64 minLifetime = 60u;

        NvFlowUint64 registeredResourceCounter = 0llu;
        NvFlowArray<RegisteredResource> registeredResources;

        NvFlowLogPrint_t logPrint = nullptr;

        NvFlowThreadPoolInterface threadPoolInterface = {};
        NvFlowThreadPool* threadPool = nullptr;
        NvFlowUint threadCount = 0u;
    };

    void addPassCompute(NvFlowContext* context, const NvFlowPassComputeParams* params);
    void addPassCopyBuffer(NvFlowContext* context, const NvFlowPassCopyBufferParams* params);
    void addPassCopyBufferToTexture(NvFlowContext* context, const NvFlowPassCopyBufferToTextureParams* params);
    void addPassCopyTextureToBuffer(NvFlowContext* context, const NvFlowPassCopyTextureToBufferParams* params);
    void addPassCopyTexture(NvFlowContext* context, const NvFlowPassCopyTextureParams* params);

    NvFlowUint64 registerBufferId(NvFlowContext* context, NvFlowBuffer* buffer);
    NvFlowUint64 registerTextureId(NvFlowContext* context, NvFlowTexture* texture);
    void unregisterBufferId(NvFlowContext* context, NvFlowUint64 bufferId);
    void unregisterTextureId(NvFlowContext* context, NvFlowUint64 textureId);
    void setResourceMinLifetime(NvFlowContext* context, NvFlowUint64 minLifetime);

    Context* context_create(DeviceQueue* deviceQueue);
    void context_destroy(Context* context);
    void context_resetNodes(Context* context);
    void context_flushNodes(Context* context);

    // The shader-visible sampler table holding exactly `samplers`, made the
    // first time it is asked for. False when the heap is full.
    NvFlowBool32 context_getSamplerTable(Context* context, Sampler* const* samplers, NvFlowUint count, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);

    /// Formats

    DXGI_FORMAT format_toDxgi(NvFlowFormat format);
    NvFlowUint format_sizeInBytes(NvFlowFormat format);

    NV_FLOW_INLINE void device_log(Device* device, NvFlowLogLevel level, const char* what, HRESULT hr)
    {
        if (device->logPrint)
        {
            device->logPrint(level, "D3D12: %s failed (0x%08x)", what, (unsigned)hr);
        }
    }
}
