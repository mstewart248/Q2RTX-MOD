/*
NVAPI's ray tracing extensions for the D3D12 renderer: NVIDIA's "RTX Mega
Geometry" (the partitioned TLAS, clustered geometry) on the RETAIL runtime.

The same hardware path as Vulkan's VK_NV_partitioned_acceleration_structure and
VK_NV_cluster_acceleration_structure, and as D3D12's own RT tier 2.0 - which
only the preview runtime has, and that only with Windows Developer Mode on
(device.c's d3d12_agility_preview). NVAPI needs neither: an NVIDIA driver
recent enough to report the caps.

nvapi64.dll is loaded at run time and the entry points come from
nvapi_QueryInterface by their public ids (nvapi_interface.h); the structs are
nvapi.h's, declared by hand in nvapi_rt.c with their sizes checked - the NVAPI
SDK is not in the tree. reflex_nvapi.c takes its entry points from here too.

No engine headers: the headless tests compile nvapi_rt.c on its own.
*/

#ifndef REFRESH_D3D12_NVAPI_RT_H
#define REFRESH_D3D12_NVAPI_RT_H

#include <stdint.h>
#include <stdbool.h>
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d12.h>

// nvapi_QueryInterface(id) after NvAPI_Initialize; NULL without an NVIDIA
// driver, or one without that entry point
void *d3d12_nvapi_query(unsigned int id);

// NvAPI_D3D12_GetRaytracingCaps' answers for this device
bool d3d12_nvapi_ptlas_supported(ID3D12Device *device);
bool d3d12_nvapi_clusters_supported(ID3D12Device *device);

// NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_FLAGS
#define D3D12_NVAPI_PTLAS_FAST_TRACE    1u
#define D3D12_NVAPI_PTLAS_FAST_BUILD    2u

// NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_INPUTS
typedef struct {
    uint32_t    flags;                  // D3D12_NVAPI_PTLAS_*
    uint32_t    instance_count;
    uint32_t    max_instances_per_partition;
    uint32_t    partition_count;
    uint32_t    max_instances_in_global;
} d3d12_nvapi_ptlas_inputs_t;

// NvAPI_D3D12_GetRaytracingPartitionedTlasIndirectPrebuildInfo
bool d3d12_nvapi_ptlas_sizes(ID3D12Device5 *device, const d3d12_nvapi_ptlas_inputs_t *inputs,
                             uint64_t *result_size, uint64_t *scratch_size);
/* NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect: `source` 0 builds from
   nothing, `source` == `dest` updates in place. `op_count` holds a uint32,
   `ops` that many NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OPs (both
   NON_PIXEL_SHADER_RESOURCE: an upload heap is). False: the driver refused. */
bool d3d12_nvapi_ptlas_build(ID3D12GraphicsCommandList4 *cmd, const d3d12_nvapi_ptlas_inputs_t *inputs,
                             D3D12_GPU_VIRTUAL_ADDRESS source, D3D12_GPU_VIRTUAL_ADDRESS dest,
                             D3D12_GPU_VIRTUAL_ADDRESS scratch, D3D12_GPU_VIRTUAL_ADDRESS op_count,
                             D3D12_GPU_VIRTUAL_ADDRESS ops);

/* CLUSTERED GEOMETRY (pt_clusters, vkpt/clusters.h): NVAPI's multi-indirect
   cluster operations. The argument arrays are vkpt/clusters.h's
   pt_cluster_args_t (CLAS from triangles) and pt_cluster_blas_args_t (BLAS
   from CLAS), which are NVAPI's structs byte for byte. */

// NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE
#define D3D12_NVAPI_CLUSTER_BLAS_FROM_CLAS          1u
#define D3D12_NVAPI_CLUSTER_CLAS_FROM_TRIANGLES     2u
// _MODE: the driver places the results in one buffer and writes their
// addresses, or writes each where the caller's address array says
#define D3D12_NVAPI_CLUSTER_IMPLICIT                0u
#define D3D12_NVAPI_CLUSTER_EXPLICIT                1u
// _FLAGS
#define D3D12_NVAPI_CLUSTER_FAST_TRACE              1u
#define D3D12_NVAPI_CLUSTER_FAST_BUILD              2u

// NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS (48 bytes)
typedef struct {
    uint32_t    max_arg_count;
    uint32_t    flags;          // D3D12_NVAPI_CLUSTER_FAST_*
    uint32_t    type;           // D3D12_NVAPI_CLUSTER_BLAS_FROM_CLAS / _CLAS_FROM_TRIANGLES
    uint32_t    mode;           // D3D12_NVAPI_CLUSTER_IMPLICIT / _EXPLICIT
    union {
        struct {                // BLAS from CLAS
            uint32_t    max_total_clas;
            uint32_t    max_clas_per_arg;
        } clas;
        struct {                // CLAS from triangles
            uint32_t    vertex_format;          // DXGI_FORMAT
            uint32_t    max_geometry_index;
            uint32_t    max_unique_geometries_per_arg;
            uint32_t    max_triangles_per_arg;  // <= 256
            uint32_t    max_vertices_per_arg;   // <= 256
            uint32_t    max_total_triangles;
            uint32_t    max_total_vertices;
            uint32_t    min_position_truncate_bits;
        } triangles;
    } u;
} d3d12_nvapi_cluster_inputs_t;

/* NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo:
   `result_size` is the whole batch's (implicit) or the most one object takes
   (explicit). */
bool d3d12_nvapi_cluster_sizes(ID3D12Device5 *device, const d3d12_nvapi_cluster_inputs_t *inputs,
                               uint64_t *result_size, uint64_t *scratch_size);
/* NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation over
   inputs->max_arg_count arguments at `args` (`arg_stride` apart,
   NON_PIXEL_SHADER_RESOURCE). `batch` = the implicit results' memory (AS state;
   0 for explicit), `destinations` = the address array (UNORDERED_ACCESS:
   written for implicit, read for explicit), `sizes` = a uint32 per result
   (UNORDERED_ACCESS, 0 = none), `scratch` a UAV. False: the driver refused. */
bool d3d12_nvapi_cluster_execute(ID3D12GraphicsCommandList4 *cmd, const d3d12_nvapi_cluster_inputs_t *inputs,
                                 D3D12_GPU_VIRTUAL_ADDRESS batch, D3D12_GPU_VIRTUAL_ADDRESS scratch,
                                 D3D12_GPU_VIRTUAL_ADDRESS destinations, D3D12_GPU_VIRTUAL_ADDRESS sizes,
                                 D3D12_GPU_VIRTUAL_ADDRESS args, uint32_t arg_stride);
/* NvAPI_D3D12_SetCreatePipelineStateOptions(ENABLE_CLUSTER_SUPPORT): every
   pipeline made after it on `device` may trace clustered BLASes - which one
   made without it must not ("undefined behaviour", nvapi.h), and which costs
   all traversal a little, so it is set only while pt_clusters is on. */
bool d3d12_nvapi_cluster_pipelines(ID3D12Device5 *device, bool enable);

#endif // REFRESH_D3D12_NVAPI_RT_H
