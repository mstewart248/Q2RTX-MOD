/*
CLUSTERED DYNAMIC GEOMETRY, API-free (both renderers): pt_clusters

NVIDIA's "RTX Mega Geometry" clusters - VK_NV_cluster_acceleration_structure
on Vulkan, NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation on
D3D12 - for vkpt's per-frame BLASes over instance_geometry.comp's triangle
lists: the dynamic runs the geometry TLAS holds (the model sections, the
viewer's weapon and models, blood). Instead of one BLAS build per run, two
operations a frame build them all:

  1. CLAS FROM TRIANGLES, implicit destinations: every run cut into clusters
     of PT_CLUSTER_TRIANGLES triangles, one cluster-level structure (CLAS)
     each, packed by the driver into one buffer, each one's address written
     to an array on the GPU
  2. BLAS FROM CLAS, explicit destinations: one BLAS per run from its slice
     of that array, at an address the CPU chose - the one the instance list
     (and the partitioned TLAS) names before the GPU has built anything

A cluster's triangles carry its index in its BLAS as their GEOMETRY INDEX, and
PrimitiveIndex() counts within the cluster, so the triangle of the run a ray
hit is geometryIndex * PT_CLUSTER_TRIANGLES + PrimitiveIndex().
get_model_index_and_prim_offset (path_tracer_hit_shaders.h) adds that for
every instance without a model index - which are all single-geometry BLASes
otherwise (geometry index 0), so nothing else moves. No ClusterID intrinsic,
no shader extension.

The runs are unindexed triangle lists (cluster k's vertex j is the run's
vertex 3 * PT_CLUSTER_TRIANGLES * k + j), so every cluster reads the same
8-bit index list 0..254, which the staging starts with.

A BLAS from CLASes references them: the CLAS memory lives as long as the BLAS
(each frame slot has its own).

The argument structs are the same bytes in NVAPI and Vulkan (each backend
checks them against its own header).

No engine headers: the headless tests compile clusters.c on its own.
*/

#ifndef VKPT_CLUSTERS_H
#define VKPT_CLUSTERS_H

#include <stdint.h>
#include <stdbool.h>
#include "shader/constants.h"   // PT_CLUSTER_TRIANGLES (the shaders' too)

#define PT_CLUSTER_VERTICES         (3 * PT_CLUSTER_TRIANGLES)    // 255: what an 8-bit index reaches
#define PT_CLUSTER_COUNT(prims)     (((prims) + PT_CLUSTER_TRIANGLES - 1) / PT_CLUSTER_TRIANGLES)
// NVAPI_..._CLUSTER_OPERATION_INDEX_FORMAT_8BIT / VK_CLUSTER_ACCELERATION_STRUCTURE_INDEX_FORMAT_8BIT_NV
#define PT_CLUSTER_INDEX_8BIT       1u
// the most runs one plan holds: vkpt's dynamic sections
#define PT_CLUSTER_MAX_BLASES       8
// both APIs' limit on a geometry index (24 bits)
#define PT_CLUSTER_MAX_GEOMETRY_INDEX   0xffffffu

// NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_MULTI_INDIRECT_TRIANGLE_CLUSTER_ARGS /
// VkClusterAccelerationStructureBuildTriangleClusterInfoNV (64 bytes)
typedef struct {
    uint32_t    cluster_id;
    uint32_t    cluster_flags;
    // bitfields, low bit first: triangles 9, vertices 9, position truncate bits 6,
    // index format 4, opacity micromap index format 4
    uint32_t    counts;
    // geometry index in the low 24 bits, geometry flags in the top 3 (0: the
    // instance's flags decide opacity, as the classic BLASes' geometry flags 0)
    uint32_t    base_geometry_index_and_flags;
    uint16_t    index_stride;
    uint16_t    vertex_stride;
    uint16_t    geometry_index_stride;
    uint16_t    omm_index_stride;
    uint64_t    indices;
    uint64_t    vertices;
    uint64_t    geometry_indices;
    uint64_t    omm_array;
    uint64_t    omm_indices;
} pt_cluster_args_t;

// NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_MULTI_INDIRECT_CLUSTER_ARGS /
// VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV (16 bytes)
typedef struct {
    uint32_t    cluster_count;
    uint32_t    stride;         // Vulkan's clusterReferencesStride (8); NVAPI's `reserved` (0)
    uint64_t    clusters;       // cluster_count CLAS addresses, 8 bytes apart
} pt_cluster_blas_args_t;

typedef char pt_cluster_args_size_check[sizeof(pt_cluster_args_t) == 64 ? 1 : -1];
typedef char pt_cluster_blas_args_size_check[sizeof(pt_cluster_blas_args_t) == 16 ? 1 : -1];

/* One frame's clustered BLASes: the runs (up to PT_CLUSTER_MAX_BLASES, in the
   caller's order) with triangles get a BLAS slot, in that order. */
typedef struct {
    uint32_t    num_runs;
    uint32_t    num_blases;
    uint32_t    num_clusters;           // all of them: step 1's argument count
    uint32_t    num_triangles;
    uint32_t    max_clusters_per_blas;  // and max_clusters_per_blas - 1 the highest geometry index
    // per run
    uint32_t    num_prims[PT_CLUSTER_MAX_BLASES];
    uint32_t    first_cluster[PT_CLUSTER_MAX_BLASES];
    uint32_t    clusters[PT_CLUSTER_MAX_BLASES];
    int         blas_slot[PT_CLUSTER_MAX_BLASES];   // -1: no triangles, no BLAS
} pt_cluster_plan_t;

/* The staging, in GPU-readable memory (an upload heap is fine): the shared
   index list, step 1's arguments, step 2's arguments, then the BLAS addresses
   step 2 writes to (which D3D12 copies to a UAV buffer: NVAPI wants its
   destination array in that state). */
typedef struct {
    uint64_t    triangle_args;      // num_clusters pt_cluster_args_t
    uint64_t    blas_args;          // num_blases pt_cluster_blas_args_t
    uint64_t    blas_addresses;     // num_blases uint64
    uint64_t    size;
} pt_cluster_layout_t;

// num_prims[i] triangles in run i (0 = none); false when it cannot be built:
// too many runs, or a geometry index past PT_CLUSTER_MAX_GEOMETRY_INDEX
bool pt_cluster_plan(pt_cluster_plan_t *plan, const uint32_t *num_prims, uint32_t num_runs);
void pt_cluster_layout(const pt_cluster_plan_t *plan, pt_cluster_layout_t *layout);
/* Writes the staging at `staging` (its GPU address `staging_gpu`): run i's
   triangles are at run_vertices[i] (float3, 12 bytes apart, 3 a triangle),
   its BLAS goes at blas_address[i], and step 1 writes the CLAS addresses to
   `clas_addresses` (8 bytes each, in cluster order). `blas_stride` is
   pt_cluster_blas_args_t's: 8 on Vulkan, 0 on NVAPI. */
void pt_cluster_stage(const pt_cluster_plan_t *plan, const pt_cluster_layout_t *layout,
                      void *staging, uint64_t staging_gpu, const uint64_t *run_vertices,
                      const uint64_t *blas_address, uint64_t clas_addresses, uint32_t blas_stride);

#endif // VKPT_CLUSTERS_H
