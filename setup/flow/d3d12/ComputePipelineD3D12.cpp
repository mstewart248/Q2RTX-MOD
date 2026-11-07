// Q2RTX addition to the NVIDIA Flow SDK (BSD-3): compute pipelines of Flow's
// D3D12 backend - vulkan/ComputePipelineVulkan.cpp with root signatures and
// descriptor tables in place of set layouts and descriptor pools. See
// DeviceD3D12.h.
//
// Flow's shader tool numbers each shader's registers per class in Slang's
// reflection order, all in space 0 (ShaderPreprocessor.cpp,
// bindingAllocatePerConfig), as Slang numbers them in the DXIL it emits. So a
// pipeline's root signature is one descriptor table of b0.., t0.., u0.., and
// one of s0.. when it samples; a mismatch fails CreateComputePipelineState.

#include "DeviceD3D12.h"

namespace NvFlowD3D12
{

NvFlowComputePipeline* createComputePipeline(NvFlowContext* contextIn, const NvFlowComputePipelineDesc* desc)
{
    auto context = cast(contextIn);
    Device* device = context->deviceQueue->device;
    auto ptr = new ComputePipeline();

    ptr->desc = *desc;
    // the descs are the caller's temporaries
    ptr->desc.bindingDescs = nullptr;

    for (NvFlowUint idx = 0u; idx < desc->numBindingDescs; idx++)
    {
        const NvFlowBindingDesc* bindingDesc = &desc->bindingDescs[idx];
        const NvFlowBindingDescD3D12& d3d12 = bindingDesc->bindingDesc.d3d12;
        NvFlowUint end = d3d12.registerBegin + (d3d12.numRegisters ? d3d12.numRegisters : 1u);
        if (d3d12.space != 0u)
        {
            device->logPrint(eNvFlowLogLevel_error, "D3D12: Flow binding in space %u, only space 0 is laid out", d3d12.space);
        }
        switch (d3d12.registerHlsl)
        {
        case eNvFlowRegisterHlsl_b: ptr->numCbv = end > ptr->numCbv ? end : ptr->numCbv; break;
        case eNvFlowRegisterHlsl_t: ptr->numSrv = end > ptr->numSrv ? end : ptr->numSrv; break;
        case eNvFlowRegisterHlsl_u: ptr->numUav = end > ptr->numUav ? end : ptr->numUav; break;
        case eNvFlowRegisterHlsl_s: ptr->numSampler = end > ptr->numSampler ? end : ptr->numSampler; break;
        default:
            device->logPrint(eNvFlowLogLevel_error, "D3D12: Flow binding %u of type %d has no HLSL register", idx, (int)bindingDesc->type);
            break;
        }
    }
    if (ptr->numSampler > kMaxSamplerTable)
    {
        device->logPrint(eNvFlowLogLevel_error, "D3D12: Flow pipeline has %u samplers, at most %u are supported", ptr->numSampler, kMaxSamplerTable);
        ptr->numSampler = kMaxSamplerTable;
    }

    // root signature
    {
        D3D12_DESCRIPTOR_RANGE viewRanges[3] = {};
        UINT numViewRanges = 0u;
        UINT offset = 0u;
        if (ptr->numCbv)
        {
            D3D12_DESCRIPTOR_RANGE& r = viewRanges[numViewRanges++];
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            r.NumDescriptors = ptr->numCbv;
            r.OffsetInDescriptorsFromTableStart = offset;
            offset += ptr->numCbv;
        }
        if (ptr->numSrv)
        {
            D3D12_DESCRIPTOR_RANGE& r = viewRanges[numViewRanges++];
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            r.NumDescriptors = ptr->numSrv;
            r.OffsetInDescriptorsFromTableStart = offset;
            offset += ptr->numSrv;
        }
        if (ptr->numUav)
        {
            D3D12_DESCRIPTOR_RANGE& r = viewRanges[numViewRanges++];
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            r.NumDescriptors = ptr->numUav;
            r.OffsetInDescriptorsFromTableStart = offset;
            offset += ptr->numUav;
        }
        D3D12_DESCRIPTOR_RANGE samplerRange = {};
        samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRange.NumDescriptors = ptr->numSampler;

        D3D12_ROOT_PARAMETER params[2] = {};
        UINT numParams = 0u;
        if (numViewRanges)
        {
            D3D12_ROOT_PARAMETER& p = params[numParams++];
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable.NumDescriptorRanges = numViewRanges;
            p.DescriptorTable.pDescriptorRanges = viewRanges;
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        if (ptr->numSampler)
        {
            D3D12_ROOT_PARAMETER& p = params[numParams++];
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable.NumDescriptorRanges = 1u;
            p.DescriptorTable.pDescriptorRanges = &samplerRange;
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = numParams;
        rsDesc.pParameters = params;

        ID3DBlob* blob = nullptr;
        ID3DBlob* error = nullptr;
        HRESULT hr = device->serializeRootSignature ?
            device->serializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error) : E_NOINTERFACE;
        if (FAILED(hr))
        {
            device->logPrint(eNvFlowLogLevel_error, "D3D12: Flow root signature: %s",
                error ? (const char*)error->GetBufferPointer() : "D3D12SerializeRootSignature unavailable");
        }
        else
        {
            hr = device->device->CreateRootSignature(0u, blob->GetBufferPointer(), blob->GetBufferSize(),
                IID_PPV_ARGS(&ptr->rootSignature));
            if (FAILED(hr))
            {
                device_log(device, eNvFlowLogLevel_error, "Flow CreateRootSignature", hr);
                ptr->rootSignature = nullptr;
            }
        }
        if (blob)
        {
            blob->Release();
        }
        if (error)
        {
            error->Release();
        }
    }

    // pipeline
    if (ptr->rootSignature && desc->bytecode.sizeInBytes > 0u)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = ptr->rootSignature;
        psoDesc.CS.pShaderBytecode = desc->bytecode.data;
        psoDesc.CS.BytecodeLength = desc->bytecode.sizeInBytes;
        HRESULT hr = device->device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&ptr->pipeline));
        if (FAILED(hr))
        {
            device_log(device, eNvFlowLogLevel_error, "Flow CreateComputePipelineState", hr);
            ptr->pipeline = nullptr;
        }
    }

    return cast(ptr);
}

void destroyComputePipeline(NvFlowContext* contextIn, NvFlowComputePipeline* pipeline)
{
    auto ptr = cast(pipeline);
    if (!ptr)
    {
        return;
    }

    if (ptr->pipeline)
    {
        ptr->pipeline->Release();
    }
    if (ptr->rootSignature)
    {
        ptr->rootSignature->Release();
    }

    delete ptr;
}

void computePipeline_dispatch(Context* context, const NvFlowPassComputeParams* params)
{
    ComputePipeline* ptr = cast(params->pipeline);
    DeviceQueue* deviceQueue = context->deviceQueue;
    Device* device = deviceQueue->device;
    ID3D12GraphicsCommandList* commandList = deviceQueue->commandList;

    if (!ptr || !ptr->pipeline)
    {
        return;
    }

    NvFlowUint3 gridDim = params->gridDim;
    if (gridDim.x == 0u || gridDim.y == 0u || gridDim.z == 0u)
    {
        return;
    }

    // every resource the pass names must exist, or the pass is dropped
    for (NvFlowUint idx = 0u; idx < params->numDescriptorWrites; idx++)
    {
        const NvFlowResource* resource = &params->resources[idx];
        NvFlowDescriptorType type = params->descriptorWrites[idx].type;
        if (type == eNvFlowDescriptorType_sampler)
        {
            continue;
        }
        NvFlowBool32 ok = NV_FLOW_FALSE;
        if (resource->bufferTransient)
        {
            Buffer* buffer = cast(resource->bufferTransient)->buffer;
            ok = buffer && buffer->resource;
        }
        else if (resource->textureTransient)
        {
            Texture* texture = cast(resource->textureTransient)->texture;
            ok = texture && texture->resource;
        }
        if (!ok)
        {
            context->logPrint(eNvFlowLogLevel_warning, "D3D12: pass %s dropped, a resource is missing",
                params->debugLabel ? params->debugLabel : "?");
            return;
        }
    }

    NvFlowUint numViews = ptr->numCbv + ptr->numSrv + ptr->numUav;
    D3D12_GPU_DESCRIPTOR_HANDLE viewTable = {};
    D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = {};
    ID3D12DescriptorHeap* viewHeap = deviceQueue->boundViewHeap;

    if (numViews)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
        if (!deviceQueue_allocateViews(deviceQueue, numViews, &viewHeap, &cpu, &viewTable))
        {
            context->logPrint(eNvFlowLogLevel_error, "D3D12: out of descriptors, pass %s dropped",
                params->debugLabel ? params->debugLabel : "?");
            return;
        }
        for (NvFlowUint idx = 0u; idx < params->numDescriptorWrites; idx++)
        {
            const NvFlowDescriptorWrite* write = &params->descriptorWrites[idx];
            const NvFlowResource* resource = &params->resources[idx];
            NvFlowUint slot = ~0u;
            NvFlowUint registerIndex = write->write.d3d12.registerIndex;
            switch (write->write.d3d12.registerHlsl)
            {
            case eNvFlowRegisterHlsl_b:
                slot = registerIndex < ptr->numCbv ? registerIndex : ~0u;
                break;
            case eNvFlowRegisterHlsl_t:
                slot = registerIndex < ptr->numSrv ? ptr->numCbv + registerIndex : ~0u;
                break;
            case eNvFlowRegisterHlsl_u:
                slot = registerIndex < ptr->numUav ? ptr->numCbv + ptr->numSrv + registerIndex : ~0u;
                break;
            default:
                break;
            }
            if (slot == ~0u)
            {
                continue;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE dst = cpu;
            dst.ptr += (SIZE_T)slot * device->viewDescriptorSize;
            if (resource->bufferTransient)
            {
                buffer_writeView(context, cast(resource->bufferTransient), write->type, dst);
            }
            else if (resource->textureTransient)
            {
                texture_writeView(context, cast(resource->textureTransient), write->type, dst);
            }
        }
    }

    if (ptr->numSampler)
    {
        Sampler* samplers[kMaxSamplerTable];
        for (NvFlowUint i = 0u; i < ptr->numSampler; i++)
        {
            samplers[i] = context->pool_samplers[0u];
        }
        for (NvFlowUint idx = 0u; idx < params->numDescriptorWrites; idx++)
        {
            const NvFlowDescriptorWrite* write = &params->descriptorWrites[idx];
            if (write->write.d3d12.registerHlsl == eNvFlowRegisterHlsl_s &&
                write->write.d3d12.registerIndex < ptr->numSampler &&
                params->resources[idx].sampler)
            {
                samplers[write->write.d3d12.registerIndex] = cast(params->resources[idx].sampler);
            }
        }
        if (!context_getSamplerTable(context, samplers, ptr->numSampler, &samplerTable))
        {
            context->logPrint(eNvFlowLogLevel_error, "D3D12: out of sampler descriptors, pass %s dropped",
                params->debugLabel ? params->debugLabel : "?");
            return;
        }
    }

    if (viewHeap != deviceQueue->boundViewHeap || !deviceQueue->heapsBound)
    {
        ID3D12DescriptorHeap* heaps[2] = {};
        UINT numHeaps = 0u;
        if (viewHeap)
        {
            heaps[numHeaps++] = viewHeap;
        }
        if (context->samplerHeap)
        {
            heaps[numHeaps++] = context->samplerHeap;
        }
        commandList->SetDescriptorHeaps(numHeaps, heaps);
        deviceQueue->boundViewHeap = viewHeap;
        deviceQueue->heapsBound = NV_FLOW_TRUE;
        // a heap change unbinds the tables, and the root signature with them
        deviceQueue->boundRootSignature = nullptr;
    }
    if (ptr->rootSignature != deviceQueue->boundRootSignature)
    {
        commandList->SetComputeRootSignature(ptr->rootSignature);
        deviceQueue->boundRootSignature = ptr->rootSignature;
    }
    commandList->SetPipelineState(ptr->pipeline);

    UINT param = 0u;
    if (numViews)
    {
        commandList->SetComputeRootDescriptorTable(param++, viewTable);
    }
    if (ptr->numSampler)
    {
        commandList->SetComputeRootDescriptorTable(param++, samplerTable);
    }

    if (gridDim.x > 65535 || gridDim.y > 65535 || gridDim.z > 65535)
    {
        context->logPrint(eNvFlowLogLevel_warning, "Dispatch gridDim of (%d, %d, %d) exceeds maximum gridDim of 65535.",
            gridDim.x, gridDim.y, gridDim.z);
    }

    commandList->Dispatch(gridDim.x, gridDim.y, gridDim.z);
}

} // end namespace
