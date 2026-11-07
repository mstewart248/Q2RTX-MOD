/*
Q2RTX addition to the NVIDIA Flow SDK (BSD-3, see extern/flow/LICENSE.md).

A Direct3D 12 backend for Flow, running on an ID3D12Device and command queue
the application already owns. Flow 2.2 ships only Vulkan and CPU backends;
eNvFlowContextApi_d3d12 is in its enums and its shader tool can emit DXIL, but
there is no device behind it. setup/flow/d3d12/ is that device, written after
source/nvflowext/vulkan/ and doing what it does; setup/build_flow.py copies it
and this header into the Flow source, compiles Flow's shaders for D3D12 as
well (Slang -> DXC), and stages this header in extern/flow/include/nvflowext.

How it runs: Flow records its passes into command lists of its own, with its
own shader-visible descriptor heaps, and executes them on `queue` at each
NvFlowDeviceInterface::flush. Within a Flow frame it transitions what it
touches; at the end of it, Flow's textures are back in
D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE and its buffers in COMMON (as
D3D12 would decay them anyway). So work the application executes on the same
queue after the flush can read them with no further synchronisation, and work
it executed before the flush is finished with them when Flow starts.

The application keeps ownership: destroying the Flow device or device manager
releases only Flow's own objects. ID3D12CommandQueue is free-threaded, so no
queue lock is needed.
*/

#ifndef NV_FLOW_D3D12_EXTERNAL_H
#define NV_FLOW_D3D12_EXTERNAL_H

#include "NvFlowExt.h"

typedef struct NvFlowD3D12ExternalDesc
{
	void* device;                   // ID3D12Device*
	void* queue;                    // ID3D12CommandQueue*, DIRECT or COMPUTE, Flow executes on
	NvFlowLogPrint_t logPrint;
} NvFlowD3D12ExternalDesc;

typedef struct NvFlowD3D12ExternalTexture
{
	void* resource;                 // ID3D12Resource*
	NvFlowUint format;              // DXGI_FORMAT of its views
	NvFlowUint state;               // D3D12_RESOURCE_STATES between Flow frames
} NvFlowD3D12ExternalTexture;

typedef struct NvFlowD3D12ExternalBuffer
{
	void* resource;                 // ID3D12Resource*
	NvFlowUint64 sizeInBytes;       // as asked for (the resource may be larger)
	NvFlowUint structureStride;     // for a structured view, 0 if it has none
	NvFlowUint format;              // DXGI_FORMAT for a typed view, 0 if it has none
	NvFlowUint state;               // D3D12_RESOURCE_STATES between Flow frames
} NvFlowD3D12ExternalBuffer;

/* Exported by nvflowext as NvFlowD3D12ExternalCreateDeviceManager etc. Use the
   device manager and device with the ordinary NvFlowDeviceInterface from
   NvFlowGetDeviceInterface(eNvFlowContextApi_d3d12): getDeviceQueue,
   getContext, flush, waitIdle, destroyDevice, destroyDeviceManager. */
typedef NvFlowDeviceManager*(NV_FLOW_ABI* PFN_NvFlowD3D12ExternalCreateDeviceManager)(const NvFlowD3D12ExternalDesc* desc);
typedef NvFlowDevice*(NV_FLOW_ABI* PFN_NvFlowD3D12ExternalCreateDevice)(NvFlowDeviceManager* manager, const NvFlowD3D12ExternalDesc* desc);

/* The native resource behind a persistent texture or buffer from
   NvFlowContextInterface::createTexture / createBuffer. */
typedef void(NV_FLOW_ABI* PFN_NvFlowD3D12ExternalGetTexture)(NvFlowTexture* texture, NvFlowD3D12ExternalTexture* out);
typedef void(NV_FLOW_ABI* PFN_NvFlowD3D12ExternalGetBuffer)(NvFlowBuffer* buffer, NvFlowD3D12ExternalBuffer* out);

#endif
