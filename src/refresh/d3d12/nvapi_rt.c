/*
NVAPI's ray tracing extensions - see nvapi_rt.h.
*/

#include "nvapi_rt.h"

#include <string.h>
#include <windows.h>

/* ---- nvapi.h, by hand (its #pragma pack(8): natural alignment here) ---- */

// MAKE_NVAPI_VERSION(type, 1)
#define NV_VERSION1(s)  ((uint32_t)sizeof(s) | (1u << 16))

// NVAPI_D3D12_RAYTRACING_CAPS_TYPE
#define NV_CAPS_CLUSTER_OPERATIONS  3
#define NV_CAPS_PARTITIONED_TLAS    4

// NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO
typedef struct {
    uint64_t    result_max_size;
    uint64_t    scratch_size;
} nv_ptlas_prebuild_info_t;

// NVAPI_GET_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO_PARAMS_V1
typedef struct {
    uint32_t                            version;
    const d3d12_nvapi_ptlas_inputs_t    *inputs;
    nv_ptlas_prebuild_info_t            *info;
} nv_ptlas_prebuild_params_t;

// NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_DESC
typedef struct {
    d3d12_nvapi_ptlas_inputs_t  inputs;
    uint64_t                    source;
    uint64_t                    dest;
    uint64_t                    scratch;
    uint64_t                    op_count;
    uint64_t                    ops;
} nv_ptlas_build_desc_t;

// NVAPI_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PARAMS_V1
typedef struct {
    uint32_t                    version;
    const nv_ptlas_build_desc_t *desc;
} nv_ptlas_build_params_t;

typedef char nv_ptlas_inputs_size_check[sizeof(d3d12_nvapi_ptlas_inputs_t) == 20 ? 1 : -1];
typedef char nv_ptlas_prebuild_params_size_check[sizeof(nv_ptlas_prebuild_params_t) == 24 ? 1 : -1];
typedef char nv_ptlas_build_desc_size_check[sizeof(nv_ptlas_build_desc_t) == 64 ? 1 : -1];
typedef char nv_ptlas_build_params_size_check[sizeof(nv_ptlas_build_params_t) == 16 ? 1 : -1];

// NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO
typedef struct {
    uint64_t    result_max_size;
    uint64_t    scratch_size;
} nv_cluster_requirements_t;

// NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS_V1
typedef struct {
    uint32_t                            version;
    const d3d12_nvapi_cluster_inputs_t  *inputs;
    nv_cluster_requirements_t           *info;
} nv_cluster_requirements_params_t;

// D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE
typedef struct {
    uint64_t    address;
    uint64_t    stride;
} nv_address_and_stride_t;

// NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC
typedef struct {
    d3d12_nvapi_cluster_inputs_t    inputs;
    uint32_t                        address_resolution_flags;
    uint64_t                        batch_result;
    uint64_t                        batch_scratch;
    nv_address_and_stride_t         destinations;
    nv_address_and_stride_t         result_sizes;
    nv_address_and_stride_t         args;
    uint64_t                        arg_count;      // 0: inputs.max_arg_count
} nv_cluster_desc_t;

// NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS_V1
typedef struct {
    uint32_t                version;
    const nv_cluster_desc_t *desc;
} nv_cluster_execute_params_t;

// NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS_V1
typedef struct {
    uint32_t    version;
    uint32_t    flags;      // NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS
} nv_pipeline_options_t;
#define NV_PIPELINE_ENABLE_CLUSTER_SUPPORT  (1u << 2)

typedef char nv_cluster_inputs_size_check[sizeof(d3d12_nvapi_cluster_inputs_t) == 48 ? 1 : -1];
typedef char nv_cluster_requirements_params_size_check[sizeof(nv_cluster_requirements_params_t) == 24 ? 1 : -1];
typedef char nv_cluster_desc_size_check[sizeof(nv_cluster_desc_t) == 128 ? 1 : -1];
typedef char nv_cluster_execute_params_size_check[sizeof(nv_cluster_execute_params_t) == 16 ? 1 : -1];
typedef char nv_pipeline_options_size_check[sizeof(nv_pipeline_options_t) == 8 ? 1 : -1];

typedef void *(*PFN_nvapi_QueryInterface)(unsigned int id);
typedef int (*PFN_NvAPI_Initialize)(void);
typedef int (*PFN_NvAPI_D3D12_GetRaytracingCaps)(ID3D12Device *device, int type, void *data, size_t size);
typedef int (*PFN_NvAPI_D3D12_GetRaytracingPartitionedTlasIndirectPrebuildInfo)(ID3D12Device5 *device,
                                                                                 const nv_ptlas_prebuild_params_t *params);
typedef int (*PFN_NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect)(ID3D12GraphicsCommandList4 *cmd,
                                                                       const nv_ptlas_build_params_t *params);
typedef int (*PFN_NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo)(
    ID3D12Device5 *device, const nv_cluster_requirements_params_t *params);
typedef int (*PFN_NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation)(
    ID3D12GraphicsCommandList4 *cmd, const nv_cluster_execute_params_t *params);
typedef int (*PFN_NvAPI_D3D12_SetCreatePipelineStateOptions)(ID3D12Device5 *device,
                                                              const nv_pipeline_options_t *options);

static struct {
    bool                        loaded;     // nvapi64.dll looked for
    PFN_nvapi_QueryInterface    query;      // NULL: no NVAPI
    PFN_NvAPI_D3D12_GetRaytracingCaps                                   get_caps;
    PFN_NvAPI_D3D12_GetRaytracingPartitionedTlasIndirectPrebuildInfo    ptlas_prebuild;
    PFN_NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect              ptlas_build;
    PFN_NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo  cluster_requirements;
    PFN_NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation              cluster_execute;
    PFN_NvAPI_D3D12_SetCreatePipelineStateOptions                               pipeline_options;
} nv;

static void load_nvapi(void)
{
    if (nv.loaded)
        return;
    nv.loaded = true;

    // the driver's: there wherever an NVIDIA driver is installed; never unloaded
    HMODULE dll = LoadLibraryA("nvapi64.dll");
    PFN_nvapi_QueryInterface query = dll
        ? (PFN_nvapi_QueryInterface)(void *)GetProcAddress(dll, "nvapi_QueryInterface") : NULL;
    PFN_NvAPI_Initialize init = query ? (PFN_NvAPI_Initialize)query(0x0150e828u) : NULL;
    // reference counted: reflex_nvapi.c and NGX initialize it too
    if (!init || init() != 0)
        return;
    nv.query = query;

    // nvapi_interface.h's ids
    nv.get_caps       = (PFN_NvAPI_D3D12_GetRaytracingCaps)query(0x85a6c2a0u);
    nv.ptlas_prebuild = (PFN_NvAPI_D3D12_GetRaytracingPartitionedTlasIndirectPrebuildInfo)query(0xcdfdc5f2u);
    nv.ptlas_build    = (PFN_NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect)query(0x7cfc6fc3u);
    nv.cluster_requirements =
        (PFN_NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo)query(0x5c9163f4u);
    nv.cluster_execute  = (PFN_NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation)query(0x67c798afu);
    nv.pipeline_options = (PFN_NvAPI_D3D12_SetCreatePipelineStateOptions)query(0x5c607a27u);
}

void *d3d12_nvapi_query(unsigned int id)
{
    load_nvapi();
    return nv.query ? nv.query(id) : NULL;
}

// NVAPI_D3D12_RAYTRACING_*_CAPS: an enum whose bit 0 is STANDARD support
static bool caps_bit(ID3D12Device *device, int type)
{
    load_nvapi();
    if (!nv.get_caps || !device)
        return false;
    int caps = 0;
    return nv.get_caps(device, type, &caps, sizeof(caps)) == 0 && (caps & 1);
}

bool d3d12_nvapi_ptlas_supported(ID3D12Device *device)
{
    return caps_bit(device, NV_CAPS_PARTITIONED_TLAS) && nv.ptlas_prebuild && nv.ptlas_build;
}

bool d3d12_nvapi_clusters_supported(ID3D12Device *device)
{
    return caps_bit(device, NV_CAPS_CLUSTER_OPERATIONS) && nv.cluster_requirements && nv.cluster_execute &&
           nv.pipeline_options;
}

bool d3d12_nvapi_ptlas_sizes(ID3D12Device5 *device, const d3d12_nvapi_ptlas_inputs_t *inputs,
                             uint64_t *result_size, uint64_t *scratch_size)
{
    *result_size = *scratch_size = 0;
    load_nvapi();
    if (!nv.ptlas_prebuild)
        return false;
    nv_ptlas_prebuild_info_t info;
    memset(&info, 0, sizeof(info));
    nv_ptlas_prebuild_params_t p;
    memset(&p, 0, sizeof(p));
    p.version = NV_VERSION1(p);
    p.inputs = inputs;
    p.info = &info;
    if (nv.ptlas_prebuild(device, &p) != 0 || !info.result_max_size)
        return false;
    *result_size = info.result_max_size;
    *scratch_size = info.scratch_size;
    return true;
}

bool d3d12_nvapi_ptlas_build(ID3D12GraphicsCommandList4 *cmd, const d3d12_nvapi_ptlas_inputs_t *inputs,
                             D3D12_GPU_VIRTUAL_ADDRESS source, D3D12_GPU_VIRTUAL_ADDRESS dest,
                             D3D12_GPU_VIRTUAL_ADDRESS scratch, D3D12_GPU_VIRTUAL_ADDRESS op_count,
                             D3D12_GPU_VIRTUAL_ADDRESS ops)
{
    if (!nv.ptlas_build)
        return false;
    nv_ptlas_build_desc_t d;
    memset(&d, 0, sizeof(d));
    d.inputs = *inputs;
    d.source = source;
    d.dest = dest;
    d.scratch = scratch;
    d.op_count = op_count;
    d.ops = ops;
    nv_ptlas_build_params_t p;
    memset(&p, 0, sizeof(p));
    p.version = NV_VERSION1(p);
    p.desc = &d;
    return nv.ptlas_build(cmd, &p) == 0;
}

bool d3d12_nvapi_cluster_sizes(ID3D12Device5 *device, const d3d12_nvapi_cluster_inputs_t *inputs,
                               uint64_t *result_size, uint64_t *scratch_size)
{
    *result_size = *scratch_size = 0;
    load_nvapi();
    if (!nv.cluster_requirements)
        return false;
    nv_cluster_requirements_t info;
    memset(&info, 0, sizeof(info));
    nv_cluster_requirements_params_t p;
    memset(&p, 0, sizeof(p));
    p.version = NV_VERSION1(p);
    p.inputs = inputs;
    p.info = &info;
    if (nv.cluster_requirements(device, &p) != 0 || !info.result_max_size)
        return false;
    *result_size = info.result_max_size;
    *scratch_size = info.scratch_size;
    return true;
}

bool d3d12_nvapi_cluster_execute(ID3D12GraphicsCommandList4 *cmd, const d3d12_nvapi_cluster_inputs_t *inputs,
                                 D3D12_GPU_VIRTUAL_ADDRESS batch, D3D12_GPU_VIRTUAL_ADDRESS scratch,
                                 D3D12_GPU_VIRTUAL_ADDRESS destinations, D3D12_GPU_VIRTUAL_ADDRESS sizes,
                                 D3D12_GPU_VIRTUAL_ADDRESS args, uint32_t arg_stride)
{
    if (!nv.cluster_execute)
        return false;
    nv_cluster_desc_t d;
    memset(&d, 0, sizeof(d));
    d.inputs = *inputs;
    d.batch_result = batch;
    d.batch_scratch = scratch;
    d.destinations.address = destinations;
    d.destinations.stride = sizeof(uint64_t);
    if (sizes) {
        d.result_sizes.address = sizes;
        d.result_sizes.stride = sizeof(uint32_t);
    }
    d.args.address = args;
    d.args.stride = arg_stride;
    nv_cluster_execute_params_t p;
    memset(&p, 0, sizeof(p));
    p.version = NV_VERSION1(p);
    p.desc = &d;
    return nv.cluster_execute(cmd, &p) == 0;
}

bool d3d12_nvapi_cluster_pipelines(ID3D12Device5 *device, bool enable)
{
    load_nvapi();
    if (!nv.pipeline_options || !device)
        return false;
    nv_pipeline_options_t o;
    memset(&o, 0, sizeof(o));
    o.version = NV_VERSION1(o);
    o.flags = enable ? NV_PIPELINE_ENABLE_CLUSTER_SUPPORT : 0;
    return nv.pipeline_options(device, &o) == 0;
}
