// Q2RTX addition to the NVIDIA Flow SDK (BSD-3): the frame graph of Flow's
// D3D12 backend - vulkan/ContextVulkan.cpp with resource states in place of
// access masks and layouts. See DeviceD3D12.h.

#include "DeviceD3D12.h"

namespace NvFlowD3D12
{

Context* context_create(DeviceQueue* deviceQueue)
{
    auto ptr = new Context();

    ptr->logPrint = deviceQueue->device->logPrint;

    ptr->deviceQueue = deviceQueue;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heapDesc.NumDescriptors = kSamplerHeapCapacity;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = deviceQueue->device->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&ptr->samplerHeap));
    if (FAILED(hr))
    {
        device_log(deviceQueue->device, eNvFlowLogLevel_error, "sampler heap", hr);
        ptr->samplerHeap = nullptr;
    }
    else
    {
        ptr->samplerHeap->SetName(L"NVIDIA Flow samplers");
    }

    NvFlowSamplerDesc samplerDesc = {};
    samplerDesc.addressModeU = eNvFlowSamplerAddressMode_border;
    samplerDesc.addressModeV = eNvFlowSamplerAddressMode_border;
    samplerDesc.addressModeW = eNvFlowSamplerAddressMode_border;
    samplerDesc.filterMode = eNvFlowSamplerFilterMode_point;

    createSampler(cast(ptr), &samplerDesc);

    ptr->profiler = profiler_create(ptr);

    NvFlowThreadPoolInterface_duplicate(&ptr->threadPoolInterface, NvFlowGetThreadPoolInterface());
    ptr->threadCount = ptr->threadPoolInterface.getDefaultThreadCount();
    ptr->threadPool = ptr->threadPoolInterface.create(ptr->threadCount, 0llu);

    return ptr;
}

void context_destroy(Context* ptr)
{
    if (ptr->threadPool)
    {
        ptr->threadPoolInterface.destroy(ptr->threadPool);
        ptr->threadPool = nullptr;
    }

    profiler_destroy(ptr, ptr->profiler);

    context_destroyBuffers(ptr);
    context_destroyTextures(ptr);
    context_destroySamplers(ptr);

    if (ptr->samplerHeap)
    {
        ptr->samplerHeap->Release();
        ptr->samplerHeap = nullptr;
    }

    delete ptr;
}

NvFlowBool32 context_getSamplerTable(Context* context, Sampler* const* samplers, NvFlowUint count, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    if (!context->samplerHeap || count > kMaxSamplerTable)
    {
        return NV_FLOW_FALSE;
    }
    for (NvFlowUint64 idx = 0u; idx < context->samplerTables.size; idx++)
    {
        const SamplerTable& table = context->samplerTables[idx];
        if (table.count != count)
        {
            continue;
        }
        NvFlowUint i = 0u;
        for (; i < count; i++)
        {
            if (table.samplers[i] != samplers[i])
            {
                break;
            }
        }
        if (i == count)
        {
            *gpu = table.gpu;
            return NV_FLOW_TRUE;
        }
    }
    if (context->samplerHeapCount + count > kSamplerHeapCapacity)
    {
        return NV_FLOW_FALSE;
    }

    Device* device = context->deviceQueue->device;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = context->samplerHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE base = context->samplerHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)context->samplerHeapCount * device->samplerDescriptorSize;
    base.ptr += (UINT64)context->samplerHeapCount * device->samplerDescriptorSize;

    SamplerTable table = {};
    table.count = count;
    table.gpu = base;
    for (NvFlowUint i = 0u; i < count; i++)
    {
        table.samplers[i] = samplers[i];
        device->device->CreateSampler(&samplers[i]->desc, cpu);
        cpu.ptr += device->samplerDescriptorSize;
    }
    context->samplerHeapCount += count;
    context->samplerTables.pushBack(table);

    *gpu = base;
    return NV_FLOW_TRUE;
}

void context_resetNodes(Context* context)
{
    context->nodes.size = 0u;
}

void context_resetNode(ContextNode* node)
{
    node->type = eContextNodeType_unknown;
    node->params.memory = ContextNodeMemoryParams{};
    node->descriptorWrites.size = 0u;
    node->resources.size = 0u;

    node->barriers.size = 0u;
}

void addPassCompute(NvFlowContext* contextIn, const NvFlowPassComputeParams* params)
{
    auto context = cast(contextIn);
    ContextNode* node = &context->nodes[context->nodes.allocateBack()];
    context_resetNode(node);

    node->type = eContextNodeType_compute;
    node->params.compute = *params;

    for (NvFlowUint descriptorIdx = 0u; descriptorIdx < params->numDescriptorWrites; descriptorIdx++)
    {
        node->descriptorWrites.pushBack(params->descriptorWrites[descriptorIdx]);
        node->resources.pushBack(params->resources[descriptorIdx]);
    }
    node->params.compute.descriptorWrites = node->descriptorWrites.data;
    node->params.compute.resources = node->resources.data;
}

void addPassCopyBuffer(NvFlowContext* contextIn, const NvFlowPassCopyBufferParams* params)
{
    auto context = cast(contextIn);
    ContextNode* node = &context->nodes[context->nodes.allocateBack()];
    context_resetNode(node);

    node->type = eContextNodeType_copyBuffer;
    node->params.copyBuffer = *params;

    NvFlowResource src = {};
    NvFlowResource dst = {};
    src.bufferTransient = params->src;
    dst.bufferTransient = params->dst;

    node->descriptorWrites.pushBack(NvFlowDescriptorWrite{ eNvFlowDescriptorType_bufferCopySrc });
    node->resources.pushBack(src);
    node->descriptorWrites.pushBack(NvFlowDescriptorWrite{ eNvFlowDescriptorType_bufferCopyDst });
    node->resources.pushBack(dst);
}

void addPassCopyBufferToTexture(NvFlowContext* contextIn, const NvFlowPassCopyBufferToTextureParams* params)
{
    auto context = cast(contextIn);
    ContextNode* node = &context->nodes[context->nodes.allocateBack()];
    context_resetNode(node);

    node->type = eContextNodeType_copyBufferToTexture;
    node->params.copyBufferToTexture = *params;

    NvFlowResource src = {};
    NvFlowResource dst = {};
    src.bufferTransient = params->src;
    dst.textureTransient = params->dst;

    node->descriptorWrites.pushBack(NvFlowDescriptorWrite{ eNvFlowDescriptorType_bufferCopySrc });
    node->resources.pushBack(src);
    node->descriptorWrites.pushBack(NvFlowDescriptorWrite{ eNvFlowDescriptorType_textureCopyDst });
    node->resources.pushBack(dst);
}

void addPassCopyTextureToBuffer(NvFlowContext* contextIn, const NvFlowPassCopyTextureToBufferParams* params)
{
    auto context = cast(contextIn);
    ContextNode* node = &context->nodes[context->nodes.allocateBack()];
    context_resetNode(node);

    node->type = eContextNodeType_copyTextureToBuffer;
    node->params.copyTextureToBuffer = *params;

    NvFlowResource src = {};
    NvFlowResource dst = {};
    src.textureTransient = params->src;
    dst.bufferTransient = params->dst;

    node->descriptorWrites.pushBack(NvFlowDescriptorWrite{ eNvFlowDescriptorType_textureCopySrc });
    node->resources.pushBack(src);
    node->descriptorWrites.pushBack(NvFlowDescriptorWrite{ eNvFlowDescriptorType_bufferCopyDst });
    node->resources.pushBack(dst);
}

void addPassCopyTexture(NvFlowContext* contextIn, const NvFlowPassCopyTextureParams* params)
{
    auto context = cast(contextIn);
    ContextNode* node = &context->nodes[context->nodes.allocateBack()];
    context_resetNode(node);

    node->type = eContextNodeType_copyTexture;
    node->params.copyTexture = *params;

    NvFlowResource src = {};
    NvFlowResource dst = {};
    src.textureTransient = params->src;
    dst.textureTransient = params->dst;

    node->descriptorWrites.pushBack(NvFlowDescriptorWrite{ eNvFlowDescriptorType_textureCopySrc });
    node->resources.pushBack(src);
    node->descriptorWrites.pushBack(NvFlowDescriptorWrite{ eNvFlowDescriptorType_textureCopyDst });
    node->resources.pushBack(dst);
}

// The state a descriptor write of `type` needs its resource in.
static D3D12_RESOURCE_STATES context_bufferState(NvFlowDescriptorType type)
{
    switch (type)
    {
    case eNvFlowDescriptorType_constantBuffer:
        return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case eNvFlowDescriptorType_structuredBuffer:
    case eNvFlowDescriptorType_buffer:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case eNvFlowDescriptorType_rwStructuredBuffer:
    case eNvFlowDescriptorType_rwBuffer:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case eNvFlowDescriptorType_indirectBuffer:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case eNvFlowDescriptorType_bufferCopySrc:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case eNvFlowDescriptorType_bufferCopyDst:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    default:
        return D3D12_RESOURCE_STATE_COMMON;
    }
}

static D3D12_RESOURCE_STATES context_textureState(NvFlowDescriptorType type)
{
    switch (type)
    {
    case eNvFlowDescriptorType_texture:
    case eNvFlowDescriptorType_textureSampler:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case eNvFlowDescriptorType_rwTexture:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case eNvFlowDescriptorType_textureCopySrc:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case eNvFlowDescriptorType_textureCopyDst:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    default:
        return D3D12_RESOURCE_STATE_COMMON;
    }
}

// Moves a resource tracked at *current to `state`, as Vulkan's backend moves
// its barriers: a transition when the state changes, a UAV barrier between
// two unordered accesses (Vulkan's write -> read/write barrier), nothing
// between two reads.
static void context_pushBarrier(NvFlowArray<D3D12_RESOURCE_BARRIER>& barriers, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES* current, D3D12_RESOURCE_STATES state)
{
    if (!resource)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    if (*current != state)
    {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = *current;
        barrier.Transition.StateAfter = state;
        barriers.pushBack(barrier);
        *current = state;
    }
    else if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        barriers.pushBack(barrier);
    }
}

void context_flushNodes(Context* context)
{
    DeviceQueue* deviceQueue = context->deviceQueue;
    ID3D12GraphicsCommandList* commandList = deviceQueue->commandList;

    // reset lifetimes
    for (NvFlowUint idx = 0u; idx < context->bufferTransients.size; idx++)
    {
        context->bufferTransients[idx]->nodeBegin = int(context->nodes.size);
        context->bufferTransients[idx]->nodeEnd = -1;
    }
    for (NvFlowUint idx = 0u; idx < context->textureTransients.size; idx++)
    {
        context->textureTransients[idx]->nodeBegin = int(context->nodes.size);
        context->textureTransients[idx]->nodeEnd = -1;
    }

    // already allocated resources begin life at -1, and if active live the whole frame
    for (NvFlowUint idx = 0u; idx < context->bufferTransients.size; idx++)
    {
        auto transient = context->bufferTransients[idx];
        if (transient->buffer)
        {
            transient->nodeBegin = -1;
            if (transient->buffer->refCount > 0)
            {
                transient->nodeEnd = int(context->nodes.size);
            }
        }
    }
    for (NvFlowUint idx = 0u; idx < context->textureTransients.size; idx++)
    {
        auto transient = context->textureTransients[idx];
        if (transient->texture)
        {
            transient->nodeBegin = -1;
            if (transient->texture->refCount > 0)
            {
                transient->nodeEnd = int(context->nodes.size);
            }
        }
    }
    // compute transient lifetimes
    for (NvFlowUint nodeIdx = 0u; nodeIdx < context->nodes.size; nodeIdx++)
    {
        ContextNode* node = &context->nodes[nodeIdx];
        for (NvFlowUint descriptorIdx = 0u; descriptorIdx < node->descriptorWrites.size; descriptorIdx++)
        {
            NvFlowResource* resource = &node->resources[descriptorIdx];
            if (resource->bufferTransient)
            {
                BufferTransient* bufferTransient = cast(resource->bufferTransient);
                if (int(nodeIdx) < bufferTransient->nodeBegin)
                {
                    bufferTransient->nodeBegin = int(nodeIdx);
                }
                if (int(nodeIdx) > bufferTransient->nodeEnd)
                {
                    bufferTransient->nodeEnd = int(nodeIdx);
                }
            }
            if (resource->textureTransient)
            {
                TextureTransient* textureTransient = cast(resource->textureTransient);
                if (int(nodeIdx) < textureTransient->nodeBegin)
                {
                    textureTransient->nodeBegin = int(nodeIdx);
                }
                if (int(nodeIdx) > textureTransient->nodeEnd)
                {
                    textureTransient->nodeEnd = int(nodeIdx);
                }
            }
        }
    }
    // extend lifetime for acquired
    for (NvFlowUint idx = 0u; idx < context->bufferAcquires.size; idx++)
    {
        if (context->bufferAcquires[idx]->bufferTransient)
        {
            context->bufferAcquires[idx]->bufferTransient->nodeEnd = int(context->nodes.size);
        }
    }
    for (NvFlowUint idx = 0u; idx < context->textureAcquires.size; idx++)
    {
        if (context->textureAcquires[idx]->textureTransient)
        {
            context->textureAcquires[idx]->textureTransient->nodeEnd = int(context->nodes.size);
        }
    }

    // reset per node transient arrays
    for (NvFlowUint nodeIdx = 0u; nodeIdx < context->nodes.size; nodeIdx++)
    {
        ContextNode* node = &context->nodes[nodeIdx];
        node->bufferTransientsCreate.size = 0u;
        node->textureTransientsCreate.size = 0u;
        node->bufferTransientsDestroy.size = 0u;
        node->textureTransientsDestroy.size = 0u;
    }
    // scatter transients to per node arrays
    for (NvFlowUint idx = 0u; idx < context->bufferTransients.size; idx++)
    {
        auto transient = context->bufferTransients[idx];
        if (transient->nodeBegin >= 0 && transient->nodeBegin < int(context->nodes.size))
        {
            context->nodes[transient->nodeBegin].bufferTransientsCreate.pushBack(transient);
        }
        if (transient->nodeEnd >= 0 && transient->nodeEnd < int(context->nodes.size))
        {
            context->nodes[transient->nodeEnd].bufferTransientsDestroy.pushBack(transient);
        }
    }
    for (NvFlowUint idx = 0u; idx < context->textureTransients.size; idx++)
    {
        auto transient = context->textureTransients[idx];
        if (transient->nodeBegin >= 0 && transient->nodeBegin < int(context->nodes.size))
        {
            context->nodes[transient->nodeBegin].textureTransientsCreate.pushBack(transient);
        }
        if (transient->nodeEnd >= 0 && transient->nodeEnd < int(context->nodes.size))
        {
            context->nodes[transient->nodeEnd].textureTransientsDestroy.pushBack(transient);
        }
    }

    // for node -1, revive already allocated resources if referenced in a different node
    for (NvFlowUint idx = 0u; idx < context->bufferTransients.size; idx++)
    {
        auto transient = context->bufferTransients[idx];
        if (transient->buffer && transient->buffer->refCount == 0 && transient->nodeEnd != -1)
        {
            transient->buffer->refCount = 1;
            transient->buffer->lastActive = deviceQueue->nextFenceValue;
        }
    }
    for (NvFlowUint idx = 0u; idx < context->textureTransients.size; idx++)
    {
        auto transient = context->textureTransients[idx];
        if (transient->texture && transient->texture->refCount == 0 && transient->nodeEnd != -1)
        {
            transient->texture->refCount = 1;
            transient->texture->lastActive = deviceQueue->nextFenceValue;
        }
    }
    // resolve transient resources
    for (NvFlowUint nodeIdx = 0u; nodeIdx < context->nodes.size; nodeIdx++)
    {
        ContextNode* node = &context->nodes[nodeIdx];

        for (NvFlowUint idx = 0u; idx < node->bufferTransientsCreate.size; idx++)
        {
            auto transient = node->bufferTransientsCreate[idx];
            if (transient->aliasBuffer)
            {
                transient->buffer = transient->aliasBuffer->buffer;
                transient->buffer->refCount++;
            }
            else
            {
                if (transient->buffer)
                {
                    context->logPrint(eNvFlowLogLevel_error, "NvFlowContext::BufferTransient double create");
                }
                transient->buffer = cast(createBuffer(cast(context), eNvFlowMemoryType_device, &transient->desc));
            }
        }
        for (NvFlowUint idx = 0u; idx < node->textureTransientsCreate.size; idx++)
        {
            auto transient = node->textureTransientsCreate[idx];
            if (transient->aliasTexture)
            {
                transient->texture = transient->aliasTexture->texture;
                transient->texture->refCount++;
            }
            else
            {
                if (transient->texture)
                {
                    context->logPrint(eNvFlowLogLevel_error, "NvFlowContext::TextureTransient double create");
                }
                transient->texture = cast(createTexture(cast(context), &transient->desc));
            }
        }

        for (NvFlowUint idx = 0u; idx < node->bufferTransientsDestroy.size; idx++)
        {
            auto transient = node->bufferTransientsDestroy[idx];
            destroyBuffer(cast(context), cast(transient->buffer));
        }
        for (NvFlowUint idx = 0u; idx < node->textureTransientsDestroy.size; idx++)
        {
            auto transient = node->textureTransientsDestroy[idx];
            destroyTexture(cast(context), cast(transient->texture));
        }
    }
    // for the final node, allocate resources needed for capture
    for (NvFlowUint idx = 0u; idx < context->bufferTransients.size; idx++)
    {
        auto transient = context->bufferTransients[idx];
        if (transient->nodeBegin == int(context->nodes.size) && transient->nodeEnd == int(context->nodes.size))
        {
            transient->buffer = cast(createBuffer(cast(context), eNvFlowMemoryType_device, &transient->desc));
        }
    }
    for (NvFlowUint idx = 0u; idx < context->textureTransients.size; idx++)
    {
        auto transient = context->textureTransients[idx];
        if (transient->nodeBegin == int(context->nodes.size) && transient->nodeEnd == int(context->nodes.size))
        {
            transient->texture = cast(createTexture(cast(context), &transient->desc));
        }
    }

    // precompute barriers
    for (NvFlowUint nodeIdx = 0u; nodeIdx < context->nodes.size; nodeIdx++)
    {
        ContextNode* node = &context->nodes[nodeIdx];
        for (NvFlowUint descriptorIdx = 0u; descriptorIdx < node->descriptorWrites.size; descriptorIdx++)
        {
            NvFlowDescriptorWrite* descriptorWrite = &node->descriptorWrites[descriptorIdx];
            NvFlowResource* resource = &node->resources[descriptorIdx];

            if (resource->bufferTransient)
            {
                Buffer* buffer = cast(resource->bufferTransient)->buffer;
                if (buffer && !buffer->fixedState)
                {
                    context_pushBarrier(node->barriers, buffer->resource, &buffer->currentState,
                        context_bufferState(descriptorWrite->type));
                }
            }
            if (resource->textureTransient)
            {
                Texture* texture = cast(resource->textureTransient)->texture;
                if (texture)
                {
                    context_pushBarrier(node->barriers, texture->resource, &texture->currentState,
                        context_textureState(descriptorWrite->type));
                }
            }
        }
    }

    profiler_beginCapture(context, context->profiler, context->nodes.size);

    // execute nodes
    for (NvFlowUint nodeIdx = 0u; nodeIdx < context->nodes.size; nodeIdx++)
    {
        ContextNode* node = &context->nodes[nodeIdx];

        if (node->barriers.size > 0u)
        {
            commandList->ResourceBarrier((UINT)node->barriers.size, node->barriers.data);
        }

        if (node->type == eContextNodeType_compute)
        {
            const auto& params = node->params.compute;
            node->label = params.debugLabel;

            computePipeline_dispatch(context, &params);
        }
        else if (node->type == eContextNodeType_copyBuffer)
        {
            const auto& params = node->params.copyBuffer;
            node->label = params.debugLabel;

            Buffer* src = cast(params.src)->buffer;
            Buffer* dst = cast(params.dst)->buffer;
            if (src && dst && src->resource && dst->resource && params.numBytes > 0u)
            {
                commandList->CopyBufferRegion(dst->resource, params.dstOffset, src->resource, params.srcOffset, params.numBytes);
            }
        }
        else if (node->type == eContextNodeType_copyBufferToTexture ||
                 node->type == eContextNodeType_copyTextureToBuffer)
        {
            // Flow's editor's frame capture only: the game never copies
            // between buffers and textures. D3D12 wants the buffer's rows
            // 256-byte aligned and its offset 512-byte aligned.
            NvFlowBool32 toTexture = node->type == eContextNodeType_copyBufferToTexture;
            node->label = toTexture ? node->params.copyBufferToTexture.debugLabel : node->params.copyTextureToBuffer.debugLabel;

            Buffer* buffer = toTexture ? cast(node->params.copyBufferToTexture.src)->buffer : cast(node->params.copyTextureToBuffer.dst)->buffer;
            Texture* texture = toTexture ? cast(node->params.copyBufferToTexture.dst)->texture : cast(node->params.copyTextureToBuffer.src)->texture;
            NvFlowUint64 bufferOffset = toTexture ? node->params.copyBufferToTexture.bufferOffset : node->params.copyTextureToBuffer.bufferOffset;
            NvFlowUint bufferRowPitch = toTexture ? node->params.copyBufferToTexture.bufferRowPitch : node->params.copyTextureToBuffer.bufferRowPitch;
            NvFlowUint bufferDepthPitch = toTexture ? node->params.copyBufferToTexture.bufferDepthPitch : node->params.copyTextureToBuffer.bufferDepthPitch;
            NvFlowUint mipLevel = toTexture ? node->params.copyBufferToTexture.textureMipLevel : node->params.copyTextureToBuffer.textureMipLevel;
            NvFlowUint3 offset = toTexture ? node->params.copyBufferToTexture.textureOffset : node->params.copyTextureToBuffer.textureOffset;
            NvFlowUint3 extent = toTexture ? node->params.copyBufferToTexture.textureExtent : node->params.copyTextureToBuffer.textureExtent;

            if (!buffer || !texture || !buffer->resource || !texture->resource)
            {
            }
            else if ((bufferRowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) != 0u ||
                     (bufferOffset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) != 0u ||
                     bufferRowPitch == 0u || (bufferDepthPitch % bufferRowPitch) != 0u)
            {
                context->logPrint(eNvFlowLogLevel_warning,
                    "D3D12: buffer/texture copy skipped, pitch %u or offset %llu not aligned",
                    bufferRowPitch, (unsigned long long)bufferOffset);
            }
            else
            {
                D3D12_TEXTURE_COPY_LOCATION bufferLoc = {};
                bufferLoc.pResource = buffer->resource;
                bufferLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                bufferLoc.PlacedFootprint.Offset = bufferOffset;
                bufferLoc.PlacedFootprint.Footprint.Format = texture->format;
                bufferLoc.PlacedFootprint.Footprint.Width = extent.x;
                bufferLoc.PlacedFootprint.Footprint.Height = bufferDepthPitch / bufferRowPitch;
                bufferLoc.PlacedFootprint.Footprint.Depth = extent.z;
                bufferLoc.PlacedFootprint.Footprint.RowPitch = bufferRowPitch;

                D3D12_TEXTURE_COPY_LOCATION textureLoc = {};
                textureLoc.pResource = texture->resource;
                textureLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                textureLoc.SubresourceIndex = mipLevel;

                D3D12_BOX box = {};
                if (toTexture)
                {
                    box.left = 0u;
                    box.top = 0u;
                    box.front = 0u;
                    box.right = extent.x;
                    box.bottom = extent.y;
                    box.back = extent.z;
                    commandList->CopyTextureRegion(&textureLoc, offset.x, offset.y, offset.z, &bufferLoc, &box);
                }
                else
                {
                    box.left = offset.x;
                    box.top = offset.y;
                    box.front = offset.z;
                    box.right = offset.x + extent.x;
                    box.bottom = offset.y + extent.y;
                    box.back = offset.z + extent.z;
                    commandList->CopyTextureRegion(&bufferLoc, 0u, 0u, 0u, &textureLoc, &box);
                }
            }
        }
        else if (node->type == eContextNodeType_copyTexture)
        {
            const auto& params = node->params.copyTexture;
            node->label = params.debugLabel;

            Texture* src = cast(params.src)->texture;
            Texture* dst = cast(params.dst)->texture;
            if (src && dst && src->resource && dst->resource)
            {
                D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                srcLoc.pResource = src->resource;
                srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                srcLoc.SubresourceIndex = params.srcMipLevel;
                D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                dstLoc.pResource = dst->resource;
                dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dstLoc.SubresourceIndex = params.dstMipLevel;
                D3D12_BOX box = {};
                box.left = params.srcOffset.x;
                box.top = params.srcOffset.y;
                box.front = params.srcOffset.z;
                box.right = params.srcOffset.x + params.extent.x;
                box.bottom = params.srcOffset.y + params.extent.y;
                box.back = params.srcOffset.z + params.extent.z;
                commandList->CopyTextureRegion(&dstLoc, params.dstOffset.x, params.dstOffset.y, params.dstOffset.z, &srcLoc, &box);
            }
        }

        profiler_timestamp(context, context->profiler, node->label);
    }

    profiler_endCapture(context, context->profiler);

    profiler_processCaptures(context, context->profiler);

    // restore resource states: textures to where the application reads them,
    // device buffers to COMMON (where D3D12 would decay them after this list)
    context->restore_barriers.size = 0u;
    for (NvFlowUint idx = 0u; idx < context->bufferTransients.size; idx++)
    {
        auto transient = context->bufferTransients[idx];
        if (transient->buffer && !transient->buffer->fixedState)
        {
            auto buffer = transient->buffer;
            if (buffer->currentState != buffer->restoreState)
            {
                context_pushBarrier(context->restore_barriers, buffer->resource, &buffer->currentState, buffer->restoreState);
            }
        }
    }
    for (NvFlowUint idx = 0u; idx < context->textureTransients.size; idx++)
    {
        auto transient = context->textureTransients[idx];
        if (transient->texture)
        {
            auto texture = transient->texture;
            if (texture->currentState != texture->restoreState)
            {
                context_pushBarrier(context->restore_barriers, texture->resource, &texture->currentState, texture->restoreState);
            }
        }
    }
    if (context->restore_barriers.size > 0u)
    {
        commandList->ResourceBarrier((UINT)context->restore_barriers.size, context->restore_barriers.data);
    }
    // (Vulkan's closing global memory barrier: the end of an
    // ExecuteCommandLists is a full barrier in D3D12.)

    // process buffer acquires
    {
        for (NvFlowUint idx = 0u; idx < context->bufferAcquires.size; idx++)
        {
            auto bufferAcquire = context->bufferAcquires[idx];
            if (!bufferAcquire->buffer)
            {
                bufferAcquire->buffer = bufferAcquire->bufferTransient->buffer;
                bufferAcquire->bufferTransient = nullptr;
                if (bufferAcquire->buffer)
                {
                    bufferAcquire->buffer->refCount++;
                }
            }
        }
    }
    // process texture acquires
    {
        for (NvFlowUint idx = 0u; idx < context->textureAcquires.size; idx++)
        {
            auto textureAcquire = context->textureAcquires[idx];
            if (!textureAcquire->texture)
            {
                textureAcquire->texture = textureAcquire->textureTransient->texture;
                textureAcquire->textureTransient = nullptr;
                if (textureAcquire->texture)
                {
                    textureAcquire->texture->refCount++;
                }
            }
        }
    }

    // reset transient arrays
    context->bufferTransients.size = 0u;
    context->textureTransients.size = 0u;

    // clean up unused resources
    for (NvFlowUint idx = 0u; idx < context->pool_buffers.size; idx++)
    {
        auto& ptr = context->pool_buffers[idx];
        if (ptr->refCount == 0 && (ptr->lastActive + context->minLifetime) <= deviceQueue->lastFenceCompleted)
        {
            buffer_destroy(context, ptr);
            ptr = nullptr;

            context->logPrint(eNvFlowLogLevel_info, "D3D12 destroy pool buffer %d", idx);

            context->pool_buffers.removeSwapPointerAtIndex(idx);
            idx--;
        }
    }
    for (NvFlowUint idx = 0u; idx < context->pool_textures.size; idx++)
    {
        auto& ptr = context->pool_textures[idx];
        if (ptr->refCount == 0 && (ptr->lastActive + context->minLifetime) <= deviceQueue->lastFenceCompleted)
        {
            texture_destroy(context, ptr);
            ptr = nullptr;

            context->logPrint(eNvFlowLogLevel_info, "D3D12 destroy pool texture %d", idx);

            context->pool_textures.removeSwapPointerAtIndex(idx);
            idx--;
        }
    }
}

/// ***************************** TimerHeap *****************************************************

void profilerCapture_init(Context* context, ProfilerCapture* ptr, NvFlowUint64 capacity)
{
    Device* device = context->deviceQueue->device;

    D3D12_QUERY_HEAP_DESC queryDesc = {};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = (UINT)capacity;
    HRESULT hr = device->device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&ptr->queryHeap));
    if (FAILED(hr))
    {
        device_log(device, eNvFlowLogLevel_warning, "timestamp query heap", hr);
        ptr->queryHeap = nullptr;
        return;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = capacity * sizeof(NvFlowUint64);
    bufDesc.Height = 1u;
    bufDesc.DepthOrArraySize = 1u;
    bufDesc.MipLevels = 1u;
    bufDesc.SampleDesc.Count = 1u;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ptr->queryBuffer));
    if (FAILED(hr))
    {
        device_log(device, eNvFlowLogLevel_warning, "timestamp readback buffer", hr);
        ptr->queryHeap->Release();
        ptr->queryHeap = nullptr;
        ptr->queryBuffer = nullptr;
        return;
    }
    ptr->queryBuffer->Map(0u, nullptr, (void**)&ptr->queryMapped);

    ptr->capacity = capacity;

    ptr->entries.reserve(capacity);
    ptr->entries.size = 0u;
}

void profilerCapture_destroy(Context* context, ProfilerCapture* ptr)
{
    if (ptr->queryBuffer)
    {
        ptr->queryBuffer->Unmap(0u, nullptr);
        ptr->queryBuffer->Release();
    }
    if (ptr->queryHeap)
    {
        ptr->queryHeap->Release();
    }

    ptr->queryHeap = nullptr;
    ptr->queryBuffer = nullptr;
    ptr->queryMapped = nullptr;
    ptr->queryFrequency = 0u;
    ptr->queryReadbackFenceVal = ~0llu;

    ptr->state = 0u;
    ptr->captureID = 0llu;
    ptr->cpuFreq = 0llu;
    ptr->capacity = 0u;

    ptr->entries.size = 0u;
}

void profilerCapture_reset(Context* context, ProfilerCapture* ptr, NvFlowUint64 minCapacity, NvFlowUint64 captureID)
{
    if (ptr->state == 0u)
    {
        if (ptr->capacity == 0u || ptr->capacity < minCapacity)
        {
            profilerCapture_destroy(context, ptr);

            NvFlowUint64 newCapacity = 128u;
            while (newCapacity < minCapacity)
            {
                newCapacity *= 2u;
            }

            profilerCapture_init(context, ptr, newCapacity);
        }

        UINT64 frequency = 0u;
        context->deviceQueue->device->queue->GetTimestampFrequency(&frequency);
        ptr->queryFrequency = frequency ? frequency : 1u;

        LARGE_INTEGER tmpCpuFreq = {};
        QueryPerformanceFrequency(&tmpCpuFreq);
        ptr->cpuFreq = tmpCpuFreq.QuadPart;

        ptr->entries.size = 0u;

        ptr->captureID = captureID;
        ptr->state = 1u;
    }
}

void profilerCapture_timestamp(Context* context, ProfilerCapture* ptr, const char* label)
{
    if (ptr->state == 1u && ptr->queryHeap && ptr->entries.size < ptr->capacity)
    {
        NvFlowUint64 entryIdx = ptr->entries.allocateBack();

        auto& entry = ptr->entries[entryIdx];

        entry.label = label;
        entry.cpuValue = 0llu;
        entry.gpuValue = 0llu;

        context->deviceQueue->commandList->EndQuery(ptr->queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)entryIdx);

        LARGE_INTEGER tmpCpuTime = {};
        QueryPerformanceCounter(&tmpCpuTime);
        entry.cpuValue = tmpCpuTime.QuadPart;
    }
}

void profilerCapture_download(Context* context, ProfilerCapture* ptr)
{
    if (ptr->state == 1u)
    {
        if (ptr->queryHeap && ptr->entries.size > 0u)
        {
            context->deviceQueue->commandList->ResolveQueryData(ptr->queryHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                0u, (UINT)ptr->entries.size, ptr->queryBuffer, 0llu);
        }

        ptr->queryReadbackFenceVal = context->deviceQueue->nextFenceValue;

        ptr->state = 2u;
    }
}

NvFlowBool32 profilerCapture_mapResults(Context* context, ProfilerCapture* ptr, NvFlowUint64* pNumEntries, NvFlowProfilerEntry** pEntries)
{
    if (ptr->state == 2u)
    {
        if (ptr->queryReadbackFenceVal > context->deviceQueue->lastFenceCompleted)
        {
            return NV_FLOW_FALSE;
        }

        for (NvFlowUint idx = 0u; idx < ptr->entries.size; idx++)
        {
            ptr->entries[idx].gpuValue = ptr->queryMapped ? ptr->queryMapped[idx] : 0u;
        }

        ptr->deltaEntries.reserve(ptr->entries.size);
        ptr->deltaEntries.size = ptr->entries.size;

        // compute time deltas
        ProfilerEntry prevEntry = {};
        if (ptr->entries.size > 0u)
        {
            prevEntry = ptr->entries[0u];
        }
        for (NvFlowUint idx = 0u; idx < ptr->entries.size; idx++)
        {
            auto entry = &ptr->entries[idx];
            auto deltaEntry = &ptr->deltaEntries[idx];

            deltaEntry->label = entry->label;
            deltaEntry->cpuDeltaTime = (float)(((double)(entry->cpuValue - prevEntry.cpuValue) / (double)(ptr->cpuFreq)));
            deltaEntry->gpuDeltaTime = (float)(((double)(entry->gpuValue - prevEntry.gpuValue) / (double)(ptr->queryFrequency)));

            prevEntry = *entry;
        }

        // map results
        if (pNumEntries)
        {
            *pNumEntries = ptr->entries.size;
        }
        if (pEntries)
        {
            *pEntries = ptr->deltaEntries.data;
        }

        ptr->state = 3u;
        return NV_FLOW_TRUE;
    }
    return NV_FLOW_FALSE;
}

void profilerCapture_unmapResults(Context* context, ProfilerCapture* ptr)
{
    if (ptr->state == 3u)
    {
        ptr->state = 0u;
    }
}

/// ***************************** Profiler *****************************************************

Profiler* profiler_create(Context* context)
{
    auto ptr = new Profiler();

    return ptr;
}

void profiler_destroy(Context* context, Profiler* ptr)
{
    for (NvFlowUint captureIndex = 0u; captureIndex < ptr->captures.size; captureIndex++)
    {
        profilerCapture_destroy(context, &ptr->captures[captureIndex]);
    }
    ptr->captures.size = 0u;

    delete ptr;
}

void profiler_beginCapture(Context* context, Profiler* ptr, NvFlowUint64 numEntries)
{
    if (!ptr->reportEntries)
    {
        return;
    }

    // account for implicit begin/end
    numEntries += 2u;

    NvFlowUint64 captureIndex = 0u;
    for (; captureIndex < ptr->captures.size; captureIndex++)
    {
        if (ptr->captures[captureIndex].state == 0u)
        {
            break;
        }
    }
    if (captureIndex == ptr->captures.size)
    {
        captureIndex = ptr->captures.allocateBack();

        profilerCapture_init(context, &ptr->captures[captureIndex], numEntries);
    }
    ptr->currentCaptureIndex = captureIndex;

    auto capture = &ptr->captures[captureIndex];

    ptr->currentCaptureID++;

    profilerCapture_reset(context, capture, numEntries, ptr->currentCaptureID);

    profilerCapture_timestamp(context, capture, "BeginCapture");
}

void profiler_endCapture(Context* context, Profiler* ptr)
{
    if (!ptr->reportEntries)
    {
        return;
    }

    if (ptr->currentCaptureIndex < ptr->captures.size)
    {
        auto capture = &ptr->captures[ptr->currentCaptureIndex];

        profilerCapture_timestamp(context, capture, "EndCapture");

        profilerCapture_download(context, capture);
    }
}

void profiler_processCaptures(Context* context, Profiler* ptr)
{
    if (!ptr->reportEntries)
    {
        return;
    }

    for (NvFlowUint64 captureIndex = 0u; captureIndex < ptr->captures.size; captureIndex++)
    {
        auto capture = &ptr->captures[captureIndex];

        NvFlowUint64 numEntries = 0u;
        NvFlowProfilerEntry* entries = nullptr;
        if (profilerCapture_mapResults(context, capture, &numEntries, &entries))
        {
            ptr->reportEntries(ptr->userdata, capture->captureID, (NvFlowUint)numEntries, entries);

            profilerCapture_unmapResults(context, capture);
        }
    }
}

void profiler_timestamp(Context* context, Profiler* ptr, const char* label)
{
    if (!ptr->reportEntries)
    {
        return;
    }

    if (ptr->currentCaptureIndex < ptr->captures.size)
    {
        auto capture = &ptr->captures[ptr->currentCaptureIndex];

        profilerCapture_timestamp(context, capture, label);
    }
}

void enableProfiler(NvFlowContext* contextIn, void* userdata, void(NV_FLOW_ABI* reportEntries)(void* userdata, NvFlowUint64 captureID, NvFlowUint numEntries, NvFlowProfilerEntry* entries))
{
    auto context = cast(contextIn);

    context->profiler->userdata = userdata;
    context->profiler->reportEntries = reportEntries;
}

void disableProfiler(NvFlowContext* contextIn)
{
    auto context = cast(contextIn);

    context->profiler->reportEntries = nullptr;
}

/// ***************************** Resource Register *****************************************************

NvFlowUint64 registerResourceId(NvFlowContext* context, NvFlowBuffer* buffer, NvFlowTexture* texture)
{
    auto ctx = cast(context);

    // check for existing
    for (NvFlowUint idx = 0u; idx < ctx->registeredResources.size; idx++)
    {
        if (ctx->registeredResources[idx].buffer == buffer && ctx->registeredResources[idx].texture == texture)
        {
            return ctx->registeredResources[idx].uid;
        }
    }

    ctx->registeredResourceCounter++;

    RegisteredResource resource = {};
    resource.buffer = buffer;
    resource.texture = texture;
    resource.uid = ctx->registeredResourceCounter;
    ctx->registeredResources.pushBack(resource);

    return resource.uid;
}

void unregisterResourceId(NvFlowContext* context, NvFlowUint64 resourceId)
{
    auto ctx = cast(context);
    NvFlowUint dstIdx = 0u;
    for (NvFlowUint srcIdx = 0u; srcIdx < ctx->registeredResources.size; srcIdx++)
    {
        if (ctx->registeredResources[srcIdx].uid != resourceId)
        {
            if (dstIdx != srcIdx)
            {
                ctx->registeredResources[dstIdx] = ctx->registeredResources[srcIdx];
            }
            dstIdx++;
        }
    }
    ctx->registeredResources.size = dstIdx;
}

NvFlowUint64 registerBufferId(NvFlowContext* context, NvFlowBuffer* buffer)
{
    return registerResourceId(context, buffer, nullptr);
}

NvFlowUint64 registerTextureId(NvFlowContext* context, NvFlowTexture* texture)
{
    return registerResourceId(context, nullptr, texture);
}

void unregisterBufferId(NvFlowContext* context, NvFlowUint64 bufferId)
{
    unregisterResourceId(context, bufferId);
}

void unregisterTextureId(NvFlowContext* context, NvFlowUint64 textureId)
{
    unregisterResourceId(context, textureId);
}

void setResourceMinLifetime(NvFlowContext* context, NvFlowUint64 minLifetime)
{
    auto ctx = cast(context);
    ctx->minLifetime = minLifetime;
}

} // end namespace
