// Q2RTX addition to the NVIDIA Flow SDK (BSD-3): buffers, textures, samplers
// and formats of Flow's D3D12 backend - vulkan/BufferVulkan.cpp,
// TextureVulkan.cpp and DeviceUtilsVulkan.cpp's formats. See DeviceD3D12.h.

#include "DeviceD3D12.h"

namespace NvFlowD3D12
{

/// ***************************** Formats *****************************************************

struct FormatElement
{
    DXGI_FORMAT dxgi;
    NvFlowUint size;
};

static FormatElement format_element(NvFlowFormat format)
{
    switch (format)
    {
    case eNvFlowFormat_r32g32b32a32_float: return { DXGI_FORMAT_R32G32B32A32_FLOAT, 16u };
    case eNvFlowFormat_r32g32b32a32_uint: return { DXGI_FORMAT_R32G32B32A32_UINT, 16u };
    case eNvFlowFormat_r32g32b32a32_sint: return { DXGI_FORMAT_R32G32B32A32_SINT, 16u };

    case eNvFlowFormat_r32g32b32_float: return { DXGI_FORMAT_R32G32B32_FLOAT, 12u };
    case eNvFlowFormat_r32g32b32_uint: return { DXGI_FORMAT_R32G32B32_UINT, 12u };
    case eNvFlowFormat_r32g32b32_sint: return { DXGI_FORMAT_R32G32B32_SINT, 12u };

    case eNvFlowFormat_r16g16b16a16_float: return { DXGI_FORMAT_R16G16B16A16_FLOAT, 8u };
    case eNvFlowFormat_r16g16b16a16_unorm: return { DXGI_FORMAT_R16G16B16A16_UNORM, 8u };
    case eNvFlowFormat_r16g16b16a16_uint: return { DXGI_FORMAT_R16G16B16A16_UINT, 8u };
    case eNvFlowFormat_r16g16b16a16_snorm: return { DXGI_FORMAT_R16G16B16A16_SNORM, 8u };
    case eNvFlowFormat_r16g16b16a16_sint: return { DXGI_FORMAT_R16G16B16A16_SINT, 8u };

    case eNvFlowFormat_r32g32_float: return { DXGI_FORMAT_R32G32_FLOAT, 8u };
    case eNvFlowFormat_r32g32_uint: return { DXGI_FORMAT_R32G32_UINT, 8u };
    case eNvFlowFormat_r32g32_sint: return { DXGI_FORMAT_R32G32_SINT, 8u };

    case eNvFlowFormat_r10g10b10a2_unorm: return { DXGI_FORMAT_R10G10B10A2_UNORM, 4u };
    case eNvFlowFormat_r10g10b10a2_uint: return { DXGI_FORMAT_R10G10B10A2_UINT, 4u };
    case eNvFlowFormat_r11g11b10_float: return { DXGI_FORMAT_R11G11B10_FLOAT, 4u };

    case eNvFlowFormat_r8g8b8a8_unorm: return { DXGI_FORMAT_R8G8B8A8_UNORM, 4u };
    case eNvFlowFormat_r8g8b8a8_unorm_srgb: return { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4u };
    case eNvFlowFormat_r8g8b8a8_uint: return { DXGI_FORMAT_R8G8B8A8_UINT, 4u };
    case eNvFlowFormat_r8g8b8a8_snorm: return { DXGI_FORMAT_R8G8B8A8_SNORM, 4u };
    case eNvFlowFormat_r8g8b8a8_sint: return { DXGI_FORMAT_R8G8B8A8_SINT, 4u };

    case eNvFlowFormat_r16g16_float: return { DXGI_FORMAT_R16G16_FLOAT, 4u };
    case eNvFlowFormat_r16g16_unorm: return { DXGI_FORMAT_R16G16_UNORM, 4u };
    case eNvFlowFormat_r16g16_uint: return { DXGI_FORMAT_R16G16_UINT, 4u };
    case eNvFlowFormat_r16g16_snorm: return { DXGI_FORMAT_R16G16_SNORM, 4u };
    case eNvFlowFormat_r16g16_sint: return { DXGI_FORMAT_R16G16_SINT, 4u };

    case eNvFlowFormat_r32_float: return { DXGI_FORMAT_R32_FLOAT, 4u };
    case eNvFlowFormat_r32_uint: return { DXGI_FORMAT_R32_UINT, 4u };
    case eNvFlowFormat_r32_sint: return { DXGI_FORMAT_R32_SINT, 4u };

    case eNvFlowFormat_r8g8_unorm: return { DXGI_FORMAT_R8G8_UNORM, 2u };
    case eNvFlowFormat_r8g8_uint: return { DXGI_FORMAT_R8G8_UINT, 2u };
    case eNvFlowFormat_r8g8_snorm: return { DXGI_FORMAT_R8G8_SNORM, 2u };
    case eNvFlowFormat_r8g8_sint: return { DXGI_FORMAT_R8G8_SINT, 2u };

    case eNvFlowFormat_r16_float: return { DXGI_FORMAT_R16_FLOAT, 2u };
    case eNvFlowFormat_r16_unorm: return { DXGI_FORMAT_R16_UNORM, 2u };
    case eNvFlowFormat_r16_uint: return { DXGI_FORMAT_R16_UINT, 2u };
    case eNvFlowFormat_r16_snorm: return { DXGI_FORMAT_R16_SNORM, 2u };
    case eNvFlowFormat_r16_sint: return { DXGI_FORMAT_R16_SINT, 2u };

    case eNvFlowFormat_r8_unorm: return { DXGI_FORMAT_R8_UNORM, 1u };
    case eNvFlowFormat_r8_uint: return { DXGI_FORMAT_R8_UINT, 1u };
    case eNvFlowFormat_r8_snorm: return { DXGI_FORMAT_R8_SNORM, 1u };
    case eNvFlowFormat_r8_sint: return { DXGI_FORMAT_R8_SINT, 1u };

    case eNvFlowFormat_b8g8r8a8_unorm: return { DXGI_FORMAT_B8G8R8A8_UNORM, 4u };
    case eNvFlowFormat_b8g8r8a8_unorm_srgb: return { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 4u };

    default: return { DXGI_FORMAT_UNKNOWN, 1u };
    }
}

DXGI_FORMAT format_toDxgi(NvFlowFormat format)
{
    return format_element(format).dxgi;
}

NvFlowUint format_sizeInBytes(NvFlowFormat format)
{
    return format_element(format).size;
}

/// ***************************** Buffer *****************************************************

static NvFlowBool32 buffer_createResource(Context* context, Buffer* ptr)
{
    Device* device = context->deviceQueue->device;

    // CBVs cover whole 256-byte units, so every buffer is a whole number of them
    ptr->resourceBytes = (ptr->desc.sizeInBytes + 255u) & ~NvFlowUint64(255u);
    if (ptr->resourceBytes == 0u)
    {
        ptr->resourceBytes = 256u;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (ptr->memoryType == eNvFlowMemoryType_upload)
    {
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        ptr->fixedState = NV_FLOW_TRUE;
    }
    else if (ptr->memoryType == eNvFlowMemoryType_readback)
    {
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        initialState = D3D12_RESOURCE_STATE_COPY_DEST;
        ptr->fixedState = NV_FLOW_TRUE;
    }
    else
    {
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (ptr->desc.usageFlags & (eNvFlowBufferUsage_rwStructuredBuffer | eNvFlowBufferUsage_rwBuffer))
        {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
    }

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width = ptr->resourceBytes;
    resDesc.Height = 1u;
    resDesc.DepthOrArraySize = 1u;
    resDesc.MipLevels = 1u;
    resDesc.SampleDesc.Count = 1u;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = flags;

    HRESULT hr = device->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        initialState, nullptr, IID_PPV_ARGS(&ptr->resource));
    if (FAILED(hr))
    {
        device->logPrint(eNvFlowLogLevel_error, "D3D12: buffer allocate failed %lld bytes (0x%08x)",
            (long long)ptr->resourceBytes, (unsigned)hr);
        ptr->resource = nullptr;
        return NV_FLOW_FALSE;
    }
    ptr->resource->SetName(L"NVIDIA Flow buffer");
    device->logPrint(eNvFlowLogLevel_info, "Buffer mem allocate %lld bytes", (long long)ptr->resourceBytes);

    ptr->allocationBytes = ptr->resourceBytes;
    device_reportMemoryAllocate(device, ptr->memoryType, ptr->allocationBytes);

    if (ptr->fixedState)
    {
        D3D12_RANGE noRead = { 0u, 0u };
        hr = ptr->resource->Map(0u, ptr->memoryType == eNvFlowMemoryType_upload ? &noRead : nullptr, &ptr->mappedData);
        if (FAILED(hr))
        {
            ptr->mappedData = nullptr;
        }
    }

    ptr->restoreState = initialState;
    ptr->currentState = initialState;
    return NV_FLOW_TRUE;
}

Buffer* buffer_create(Context* context, NvFlowMemoryType memoryType, const NvFlowBufferDesc* desc)
{
    auto ptr = new Buffer();

    ptr->desc = *desc;
    ptr->memoryType = memoryType;

    if (!buffer_createResource(context, ptr))
    {
        delete ptr;
        return nullptr;
    }

    return ptr;
}

void buffer_destroy(Context* context, Buffer* ptr)
{
    if (ptr->resource)
    {
        if (ptr->mappedData)
        {
            ptr->resource->Unmap(0u, nullptr);
        }
        ptr->resource->Release();
        device_reportMemoryFree(context->deviceQueue->device, ptr->memoryType, ptr->allocationBytes);
    }
    ptr->mappedData = nullptr;
    ptr->resource = nullptr;

    delete ptr;
}

void buffer_writeView(Context* context, BufferTransient* transient, NvFlowDescriptorType type, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    ID3D12Device* device = context->deviceQueue->device->device;
    Buffer* ptr = transient->buffer;

    NvFlowFormat format = transient->aliasFormat != eNvFlowFormat_unknown ? transient->aliasFormat : ptr->desc.format;
    NvFlowUint stride = transient->aliasStructureStride != ~0u ? transient->aliasStructureStride : ptr->desc.structureStride;

    if (type == eNvFlowDescriptorType_constantBuffer)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
        desc.BufferLocation = ptr->resource->GetGPUVirtualAddress();
        desc.SizeInBytes = (UINT)ptr->resourceBytes;
        if (desc.SizeInBytes > D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u)
        {
            desc.SizeInBytes = D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u;
        }
        device->CreateConstantBufferView(&desc, dst);
    }
    else if (type == eNvFlowDescriptorType_structuredBuffer || type == eNvFlowDescriptorType_rwStructuredBuffer)
    {
        if (stride == 0u)
        {
            stride = 4u;
        }
        UINT numElements = (UINT)(ptr->desc.sizeInBytes / stride);
        if (type == eNvFlowDescriptorType_structuredBuffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Buffer.NumElements = numElements;
            desc.Buffer.StructureByteStride = stride;
            device->CreateShaderResourceView(ptr->resource, &desc, dst);
        }
        else
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = numElements;
            desc.Buffer.StructureByteStride = stride;
            device->CreateUnorderedAccessView(ptr->resource, nullptr, &desc, dst);
        }
    }
    else if (type == eNvFlowDescriptorType_buffer || type == eNvFlowDescriptorType_rwBuffer)
    {
        DXGI_FORMAT dxgi = format_toDxgi(format);
        if (dxgi == DXGI_FORMAT_UNKNOWN)
        {
            dxgi = DXGI_FORMAT_R32_UINT;
            format = eNvFlowFormat_r32_uint;
        }
        UINT numElements = (UINT)(ptr->desc.sizeInBytes / format_sizeInBytes(format));
        if (type == eNvFlowDescriptorType_buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format = dxgi;
            desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Buffer.NumElements = numElements;
            device->CreateShaderResourceView(ptr->resource, &desc, dst);
        }
        else
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
            desc.Format = dxgi;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = numElements;
            device->CreateUnorderedAccessView(ptr->resource, nullptr, &desc, dst);
        }
    }
}

NvFlowBool32 bufferDesc_compare(const NvFlowBufferDesc* a, const NvFlowBufferDesc* b)
{
    if (a->usageFlags == b->usageFlags &&
        a->format == b->format &&
        a->structureStride == b->structureStride &&
        a->sizeInBytes == b->sizeInBytes)
    {
        return NV_FLOW_TRUE;
    }
    return NV_FLOW_FALSE;
}

NvFlowBuffer* createBuffer(NvFlowContext* contextIn, NvFlowMemoryType memoryType, const NvFlowBufferDesc* desc)
{
    auto context = cast(contextIn);

    for (NvFlowUint idx = 0u; idx < context->pool_buffers.size; idx++)
    {
        auto ptr = context->pool_buffers[idx];
        if (ptr && ptr->refCount == 0 && bufferDesc_compare(&ptr->desc, desc) && ptr->memoryType == memoryType)
        {
            ptr->refCount = 1;
            ptr->lastActive = context->deviceQueue->nextFenceValue;
            return cast(ptr);
        }
    }

    auto ptr = buffer_create(context, memoryType, desc);
    if (!ptr)
    {
        return nullptr;
    }

    ptr->refCount = 1;
    ptr->lastActive = context->deviceQueue->nextFenceValue;

    context->pool_buffers.pushBack(ptr);

    return cast(ptr);
}

NvFlowBuffer* createBufferFromExternalHandle(NvFlowContext* contextIn, const NvFlowBufferDesc* desc, const NvFlowInteropHandle* interopHandle)
{
    // not supported: the application shares Flow's device instead
    return nullptr;
}

void destroyBuffer(NvFlowContext* contextIn, NvFlowBuffer* buffer)
{
    auto context = cast(contextIn);
    auto ptr = cast(buffer);
    if (!ptr)
    {
        return;
    }

    if (ptr->refCount <= 0)
    {
        context->logPrint(eNvFlowLogLevel_error, "NvFlowContext::destroyBuffer() called on inactive buffer %p", buffer);
    }

    ptr->refCount--;
    ptr->lastActive = context->deviceQueue->nextFenceValue;
}

void context_destroyBuffers(Context* context)
{
    context->bufferTransients.deletePointers();
    context->bufferAcquires.deletePointers();

    for (NvFlowUint idx = 0u; idx < context->pool_buffers.size; idx++)
    {
        auto& ptr = context->pool_buffers[idx];
        buffer_destroy(context, ptr);
        ptr = nullptr;
    }
}

NvFlowBufferTransient* getBufferTransient(NvFlowContext* contextIn, const NvFlowBufferDesc* desc)
{
    auto context = cast(contextIn);
    auto ptr = context->bufferTransients.allocateBackPointer();
    ptr->desc = *desc;
    ptr->buffer = nullptr;
    ptr->aliasBuffer = nullptr;
    ptr->aliasFormat = eNvFlowFormat_unknown;
    ptr->aliasStructureStride = ~0u;
    ptr->nodeBegin = 0;
    ptr->nodeEnd = 0;
    return cast(ptr);
}

NvFlowBufferTransient* registerBufferAsTransient(NvFlowContext* contextIn, NvFlowBuffer* buffer)
{
    auto context = cast(contextIn);
    auto ptr = context->bufferTransients.allocateBackPointer();
    ptr->desc = cast(buffer)->desc;
    ptr->buffer = cast(buffer);
    ptr->aliasBuffer = nullptr;
    ptr->aliasFormat = eNvFlowFormat_unknown;
    ptr->aliasStructureStride = ~0u;
    ptr->nodeBegin = 0;
    ptr->nodeEnd = 0;
    return cast(ptr);
}

NvFlowBufferTransient* aliasBufferTransient(NvFlowContext* contextIn, NvFlowBufferTransient* buffer, NvFlowFormat format, NvFlowUint structureStride)
{
    auto context = cast(contextIn);
    auto ptr = context->bufferTransients.allocateBackPointer();
    ptr->desc = cast(buffer)->desc;
    ptr->buffer = nullptr;
    ptr->aliasBuffer = cast(buffer);
    ptr->aliasFormat = format;
    ptr->aliasStructureStride = structureStride;
    ptr->nodeBegin = 0;
    ptr->nodeEnd = 0;
    return cast(ptr);
}

NvFlowBufferAcquire* enqueueAcquireBuffer(NvFlowContext* contextIn, NvFlowBufferTransient* bufferTransient)
{
    auto context = cast(contextIn);
    auto ptr = context->bufferAcquires.allocateBackPointer();
    ptr->bufferTransient = cast(bufferTransient);
    ptr->buffer = nullptr;
    return cast(ptr);
}

NvFlowBool32 getAcquiredBuffer(NvFlowContext* contextIn, NvFlowBufferAcquire* acquire, NvFlowBuffer** outBuffer)
{
    auto context = cast(contextIn);
    auto ptr = cast(acquire);
    if (ptr->buffer)
    {
        *outBuffer = cast(ptr->buffer);

        // remove from acquire array
        context->bufferAcquires.removeSwapPointer(ptr);

        return NV_FLOW_TRUE;
    }
    return NV_FLOW_FALSE;
}

void* mapBuffer(NvFlowContext* context, NvFlowBuffer* buffer)
{
    return (cast(buffer))->mappedData;
}

void unmapBuffer(NvFlowContext* context, NvFlowBuffer* buffer)
{
    // NOP: upload and readback buffers stay mapped
}

NvFlowBufferTransient* getBufferTransientById(NvFlowContext* context, NvFlowUint64 bufferId)
{
    auto ctx = cast(context);
    for (NvFlowUint idx = 0u; idx < ctx->registeredResources.size; idx++)
    {
        if (ctx->registeredResources[idx].uid == bufferId)
        {
            if (ctx->registeredResources[idx].buffer)
            {
                return registerBufferAsTransient(context, ctx->registeredResources[idx].buffer);
            }
        }
    }
    return nullptr;
}

void device_getBufferExternalHandle(NvFlowContext* context, NvFlowBuffer* buffer, void* dstHandle, NvFlowUint64 dstHandleSize, NvFlowUint64* pBufferSizeInBytes)
{
    memset(dstHandle, 0, dstHandleSize);
    if (pBufferSizeInBytes)
    {
        *pBufferSizeInBytes = 0u;
    }
}

void device_closeBufferExternalHandle(NvFlowContext* context, NvFlowBuffer* buffer, const void* srcHandle, NvFlowUint64 srcHandleSize)
{
}

void getBufferExternalHandle(NvFlowContext* context, NvFlowBuffer* buffer, NvFlowInteropHandle* dstHandle)
{
    dstHandle->type = eNvFlowInteropHandleType_unknown;
    dstHandle->value = 0u;
    dstHandle->resourceSizeInBytes = 0u;
}

void closeBufferExternalHandle(NvFlowContext* context, NvFlowBuffer* buffer, const NvFlowInteropHandle* srcHandle)
{
}

/// ***************************** Texture *****************************************************

static void texture_descClamping(Texture* ptr)
{
    if (ptr->desc.mipLevels == 0u)
    {
        ptr->desc.mipLevels = 1u;
    }
    if (ptr->desc.textureType == eNvFlowTextureType_1d)
    {
        ptr->desc.height = 1u;
        ptr->desc.depth = 1u;
    }
    else if (ptr->desc.textureType == eNvFlowTextureType_2d)
    {
        ptr->desc.depth = 1u;
    }
}

Texture* texture_create(Context* context, const NvFlowTextureDesc* desc)
{
    Device* device = context->deviceQueue->device;
    auto ptr = new Texture();

    ptr->desc = *desc;
    texture_descClamping(ptr);
    ptr->format = format_toDxgi(ptr->desc.format);

    D3D12_RESOURCE_DESC resDesc = {};
    if (ptr->desc.textureType == eNvFlowTextureType_1d)
    {
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        resDesc.DepthOrArraySize = 1u;
    }
    else if (ptr->desc.textureType == eNvFlowTextureType_2d)
    {
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.DepthOrArraySize = 1u;
    }
    else
    {
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        resDesc.DepthOrArraySize = (UINT16)ptr->desc.depth;
    }
    resDesc.Width = ptr->desc.width;
    resDesc.Height = ptr->desc.height;
    resDesc.MipLevels = (UINT16)ptr->desc.mipLevels;
    resDesc.Format = ptr->format;
    resDesc.SampleDesc.Count = 1u;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (ptr->desc.usageFlags & eNvFlowTextureUsage_rwTexture)
    {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ptr->resource));
    if (FAILED(hr))
    {
        device->logPrint(eNvFlowLogLevel_error, "D3D12: texture allocate failed %ux%ux%u format %d (0x%08x)",
            ptr->desc.width, ptr->desc.height, ptr->desc.depth, (int)ptr->format, (unsigned)hr);
        ptr->resource = nullptr;
    }
    else
    {
        ptr->resource->SetName(L"NVIDIA Flow texture");
        D3D12_RESOURCE_ALLOCATION_INFO info = device->device->GetResourceAllocationInfo(0u, 1u, &resDesc);
        ptr->allocationBytes = info.SizeInBytes;
        device_reportMemoryAllocate(device, eNvFlowMemoryType_device, ptr->allocationBytes);
        device->logPrint(eNvFlowLogLevel_info, "Texture vidmem allocate %lld bytes", (long long)ptr->allocationBytes);
    }

    ptr->restoreState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ptr->currentState = D3D12_RESOURCE_STATE_COMMON;

    return ptr;
}

void texture_destroy(Context* context, Texture* ptr)
{
    if (ptr->resource)
    {
        ptr->resource->Release();
        device_reportMemoryFree(context->deviceQueue->device, eNvFlowMemoryType_device, ptr->allocationBytes);
    }
    ptr->resource = nullptr;

    delete ptr;
}

void texture_writeView(Context* context, TextureTransient* transient, NvFlowDescriptorType type, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    ID3D12Device* device = context->deviceQueue->device->device;
    Texture* ptr = transient->texture;

    // Like Vulkan's backend, views are in the texture's own format: aliasing
    // formats is reported unsupported (isFeatureSupported), since D3D12 would
    // need typeless resources for it.
    DXGI_FORMAT format = ptr->format;

    if (type == eNvFlowDescriptorType_rwTexture)
    {
        // mip 0, as Vulkan's imageViewVk_mipLevel
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.Format = format;
        if (ptr->desc.textureType == eNvFlowTextureType_1d)
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        }
        else if (ptr->desc.textureType == eNvFlowTextureType_2d)
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        else
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            desc.Texture3D.WSize = ptr->desc.depth;
        }
        device->CreateUnorderedAccessView(ptr->resource, nullptr, &desc, dst);
    }
    else
    {
        // every mip, as Vulkan's imageViewVk_all
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = format;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (ptr->desc.textureType == eNvFlowTextureType_1d)
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            desc.Texture1D.MipLevels = ptr->desc.mipLevels;
        }
        else if (ptr->desc.textureType == eNvFlowTextureType_2d)
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = ptr->desc.mipLevels;
        }
        else
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            desc.Texture3D.MipLevels = ptr->desc.mipLevels;
        }
        device->CreateShaderResourceView(ptr->resource, &desc, dst);
    }
}

NvFlowBool32 textureDesc_compare(const NvFlowTextureDesc* a, const NvFlowTextureDesc* b)
{
    if (a->textureType == b->textureType &&
        a->usageFlags == b->usageFlags &&
        a->format == b->format &&
        a->width == b->width &&
        a->height == b->height &&
        a->depth == b->depth &&
        a->mipLevels == b->mipLevels &&
        a->optimizedClearValue.x == b->optimizedClearValue.x &&
        a->optimizedClearValue.y == b->optimizedClearValue.y &&
        a->optimizedClearValue.z == b->optimizedClearValue.z &&
        a->optimizedClearValue.w == b->optimizedClearValue.w )
    {
        return NV_FLOW_TRUE;
    }
    return NV_FLOW_FALSE;
}

NvFlowTexture* createTexture(NvFlowContext* contextIn, const NvFlowTextureDesc* desc)
{
    auto context = cast(contextIn);

    for (NvFlowUint idx = 0u; idx < context->pool_textures.size; idx++)
    {
        auto ptr = context->pool_textures[idx];
        if (ptr && ptr->refCount == 0 && textureDesc_compare(&ptr->desc, desc))
        {
            ptr->refCount = 1;
            ptr->lastActive = context->deviceQueue->nextFenceValue;
            return cast(ptr);
        }
    }

    auto ptr = texture_create(context, desc);

    ptr->refCount = 1;
    ptr->lastActive = context->deviceQueue->nextFenceValue;

    context->pool_textures.pushBack(ptr);

    return cast(ptr);
}

void destroyTexture(NvFlowContext* contextIn, NvFlowTexture* texture)
{
    auto context = cast(contextIn);
    auto ptr = cast(texture);
    if (!ptr)
    {
        return;
    }

    if (ptr->refCount <= 0)
    {
        context->logPrint(eNvFlowLogLevel_error, "NvFlowContext::destroyTexture() called on inactive texture %p", texture);
    }

    ptr->refCount--;
    ptr->lastActive = context->deviceQueue->nextFenceValue;
}

void context_destroyTextures(Context* context)
{
    context->textureTransients.deletePointers();
    context->textureAcquires.deletePointers();

    for (NvFlowUint idx = 0u; idx < context->pool_textures.size; idx++)
    {
        auto& ptr = context->pool_textures[idx];
        texture_destroy(context, ptr);
        ptr = nullptr;
    }
}

NvFlowTextureTransient* getTextureTransient(NvFlowContext* contextIn, const NvFlowTextureDesc* desc)
{
    auto context = cast(contextIn);
    auto ptr = context->textureTransients.allocateBackPointer();
    ptr->desc = *desc;
    ptr->texture = nullptr;
    ptr->aliasTexture = nullptr;
    ptr->aliasFormat = eNvFlowFormat_unknown;
    ptr->nodeBegin = 0;
    ptr->nodeEnd = 0;
    return cast(ptr);
}

NvFlowTextureTransient* registerTextureAsTransient(NvFlowContext* contextIn, NvFlowTexture* texture)
{
    auto context = cast(contextIn);
    auto ptr = context->textureTransients.allocateBackPointer();
    ptr->desc = cast(texture)->desc;
    ptr->texture = cast(texture);
    ptr->aliasTexture = nullptr;
    ptr->aliasFormat = eNvFlowFormat_unknown;
    ptr->nodeBegin = 0;
    ptr->nodeEnd = 0;
    return cast(ptr);
}

NvFlowTextureTransient* aliasTextureTransient(NvFlowContext* contextIn, NvFlowTextureTransient* texture, NvFlowFormat format)
{
    auto context = cast(contextIn);
    auto ptr = context->textureTransients.allocateBackPointer();
    ptr->desc = cast(texture)->desc;
    ptr->texture = nullptr;
    ptr->aliasTexture = cast(texture);
    ptr->aliasFormat = format;
    ptr->nodeBegin = 0;
    ptr->nodeEnd = 0;
    return cast(ptr);
}

NvFlowTextureAcquire* enqueueAcquireTexture(NvFlowContext* contextIn, NvFlowTextureTransient* textureTransient)
{
    auto context = cast(contextIn);
    auto ptr = context->textureAcquires.allocateBackPointer();
    ptr->textureTransient = cast(textureTransient);
    ptr->texture = nullptr;
    return cast(ptr);
}

NvFlowBool32 getAcquiredTexture(NvFlowContext* contextIn, NvFlowTextureAcquire* acquire, NvFlowTexture** outTexture)
{
    auto context = cast(contextIn);
    auto ptr = cast(acquire);
    if (ptr->texture)
    {
        *outTexture = cast(ptr->texture);

        // remove from acquire array
        context->textureAcquires.removeSwapPointer(ptr);

        return NV_FLOW_TRUE;
    }
    return NV_FLOW_FALSE;
}

NvFlowTextureTransient* getTextureTransientById(NvFlowContext* context, NvFlowUint64 textureId)
{
    auto ctx = cast(context);
    for (NvFlowUint idx = 0u; idx < ctx->registeredResources.size; idx++)
    {
        if (ctx->registeredResources[idx].uid == textureId)
        {
            if (ctx->registeredResources[idx].texture)
            {
                return registerTextureAsTransient(context, ctx->registeredResources[idx].texture);
            }
        }
    }
    return nullptr;
}

/// ***************************** Samplers *****************************************************

static D3D12_TEXTURE_ADDRESS_MODE sampler_convertAddressMode(NvFlowSamplerAddressMode addressMode)
{
    switch (addressMode)
    {
    case eNvFlowSamplerAddressMode_wrap:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case eNvFlowSamplerAddressMode_clamp:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case eNvFlowSamplerAddressMode_mirror:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case eNvFlowSamplerAddressMode_border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
}

Sampler* sampler_create(Context* context, const NvFlowSamplerDesc* desc)
{
    auto ptr = new Sampler();

    // point or linear in min, mag and mip alike, as Vulkan's backend
    ptr->desc.Filter = desc->filterMode == eNvFlowSamplerFilterMode_linear ?
        D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    ptr->desc.AddressU = sampler_convertAddressMode(desc->addressModeU);
    ptr->desc.AddressV = sampler_convertAddressMode(desc->addressModeV);
    ptr->desc.AddressW = sampler_convertAddressMode(desc->addressModeW);
    ptr->desc.MipLODBias = 0.f;
    ptr->desc.MaxAnisotropy = 1u;
    ptr->desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    // transparent black, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
    ptr->desc.BorderColor[0] = 0.f;
    ptr->desc.BorderColor[1] = 0.f;
    ptr->desc.BorderColor[2] = 0.f;
    ptr->desc.BorderColor[3] = 0.f;
    ptr->desc.MinLOD = 0.f;
    ptr->desc.MaxLOD = D3D12_FLOAT32_MAX;

    return ptr;
}

void sampler_destroy(Context* context, Sampler* ptr)
{
    delete ptr;
}

NvFlowSampler* createSampler(NvFlowContext* contextIn, const NvFlowSamplerDesc* desc)
{
    auto context = cast(contextIn);

    auto ptr = sampler_create(context, desc);

    ptr->isActive = NV_FLOW_TRUE;
    ptr->lastActive = context->deviceQueue->nextFenceValue;

    context->pool_samplers.pushBack(ptr);

    return cast(ptr);
}

NvFlowSampler* getDefaultSampler(NvFlowContext* contextIn)
{
    auto context = cast(contextIn);
    return cast(context->pool_samplers[0u]);
}

void destroySampler(NvFlowContext* contextIn, NvFlowSampler* sampler)
{
    auto context = cast(contextIn);
    auto ptr = cast(sampler);

    ptr->isActive = NV_FLOW_FALSE;
    ptr->lastActive = context->deviceQueue->nextFenceValue;
}

void context_destroySamplers(Context* context)
{
    for (NvFlowUint idx = 0u; idx < context->pool_samplers.size; idx++)
    {
        auto& ptr = context->pool_samplers[idx];
        sampler_destroy(context, ptr);
        ptr = nullptr;
    }
}

} // end namespace
