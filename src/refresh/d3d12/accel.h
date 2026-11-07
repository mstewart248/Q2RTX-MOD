/*
THE ACCELERATION STRUCTURE LAYER

What vkpt's path_tracer.c does with VK_KHR_acceleration_structure, on DXR, in
the shapes vkpt's geometry already has:

  * GEOMETRY is float3 positions at a 12-byte stride, either a plain triangle
    list (the BSP world, instance_geometry's output) or uint16-indexed (static
    models) - vkpt_pt_create_accel_bottom's two cases. Geometry flags stay 0
    as vkpt's do: opacity comes from the instance flags, whose bits are the
    same in both APIs (TRIANGLE_CULL_DISABLE 1, FRONT_COUNTERCLOCKWISE 2,
    FORCE_OPAQUE 4, FORCE_NON_OPAQUE 8).

  * INSTANCES are D3D12_RAYTRACING_INSTANCE_DESC, which is byte for byte
    vkpt's QvkGeometryInstance_t / VkAccelerationStructureInstanceKHR (3x4
    row-major transform, 24-bit id + 8-bit mask, 24-bit contribution + 8-bit
    flags, 64-bit address) - so vkpt's instance list can be shared once the
    address it writes is the D3D12 BLAS's. (QvkGeometryInstance_t is private
    to path_tracer.c; the size check goes in with the shared instance list.)

  * MEMORY is a pool: one buffer in the RAYTRACING_ACCELERATION_STRUCTURE
    state (which a buffer is created in and never leaves), bump-allocated at
    the 256-byte alignment DXR wants, reset as a whole - per map for the
    static world, per frame for the dynamic BLASes and the TLAS.

This is the CLASSIC backend, plus the partitioned TLAS and the clustered
per-frame BLASes (below) behind the same build points - see ROADMAP.md.

No engine headers: the headless tests compile accel.c on its own.
*/

#ifndef REFRESH_D3D12_ACCEL_H
#define REFRESH_D3D12_ACCEL_H

#include <stdint.h>
#include <stdbool.h>
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d12.h>

#include "nvapi_rt.h"
#include "../vkpt/clusters.h"

#define D3D12_AS_ALIGNMENT  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT  // 256

typedef struct {
    D3D12_GPU_VIRTUAL_ADDRESS   vertices;       // float3 positions, 12-byte stride
    uint32_t                    num_vertices;
    D3D12_GPU_VIRTUAL_ADDRESS   indices;        // uint16; 0 = a plain triangle list
    uint32_t                    num_indices;
    // non-zero: not triangles but num_aabbs boxes (D3D12_RAYTRACING_AABB, 24 bytes:
    // vkpt's VkAabbPositionsKHR) - vkpt_pt_create_accel_bottom_aabb's beams
    D3D12_GPU_VIRTUAL_ADDRESS   aabbs;
    uint32_t                    num_aabbs;
    // the indices are uint32 (sound_trace.c's scene), not uint16
    bool                        indices_32;
} d3d12_as_triangles_t;

typedef struct {
    ID3D12Resource  *buffer;
    uint64_t        size;
    uint64_t        used;
} d3d12_as_pool_t;

typedef struct {
    uint64_t        result;         // the structure itself
    uint64_t        scratch;        // to build it
    uint64_t        update_scratch; // to refit it (ALLOW_UPDATE builds)
} d3d12_as_sizes_t;

// A pool of `size` bytes of acceleration structure memory (DEFAULT heap, UAV).
bool d3d12_as_pool_create(d3d12_as_pool_t *pool, ID3D12Device *device, uint64_t size, const wchar_t *name);
void d3d12_as_pool_destroy(d3d12_as_pool_t *pool);
// everything allocated from it is dead (the GPU must be past its last use)
void d3d12_as_pool_reset(d3d12_as_pool_t *pool);
// a 256-aligned piece, or 0 when the pool is full
D3D12_GPU_VIRTUAL_ADDRESS d3d12_as_pool_alloc(d3d12_as_pool_t *pool, uint64_t size);

// What a BLAS of these geometries needs; false if the driver gives no answer.
bool d3d12_blas_sizes(ID3D12Device5 *device, const d3d12_as_triangles_t *geometry, int num_geometries,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, d3d12_as_sizes_t *sizes);
// Records the build; `source` non-zero = refit that BLAS (flags must include PERFORM_UPDATE).
void d3d12_blas_build(ID3D12GraphicsCommandList4 *cmd, const d3d12_as_triangles_t *geometry, int num_geometries,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, D3D12_GPU_VIRTUAL_ADDRESS dest,
                      D3D12_GPU_VIRTUAL_ADDRESS scratch, D3D12_GPU_VIRTUAL_ADDRESS source);

bool d3d12_tlas_sizes(ID3D12Device5 *device, uint32_t num_instances,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, d3d12_as_sizes_t *sizes);
// `instances` = num_instances D3D12_RAYTRACING_INSTANCE_DESCs, 16-byte aligned
void d3d12_tlas_build(ID3D12GraphicsCommandList4 *cmd, D3D12_GPU_VIRTUAL_ADDRESS instances, uint32_t num_instances,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, D3D12_GPU_VIRTUAL_ADDRESS dest,
                      D3D12_GPU_VIRTUAL_ADDRESS scratch);

// The builds recorded so far finish before anything after reads the pool
// (a TLAS build reading BLASes, a shader tracing the TLAS, a scratch reuse).
// NULL = every UAV access (scratch reuse lives in another buffer).
void d3d12_as_barrier(ID3D12GraphicsCommandList4 *cmd, const d3d12_as_pool_t *pool);

/* ------------------------------------------------------------------------- */
/* THE PER-FRAME BLASes                                                       */
/* ------------------------------------------------------------------------- */

/* vkpt_pt_create_all_dynamic's BLASes, rebuilt every frame into memory of
   the frame slot's own: the model sections and explosions (runs of
   instance_geometry.comp's positions), the particles and sprites (indexed
   quads) and the beams (AABBs). They are PLANNED before they are recorded -
   sized and given their addresses - because the instance list that names
   those addresses is the CPU's, and is finished first.

   CLUSTERED (pt_clusters, vkpt/clusters.h): a plain triangle list marked
   `clustered` is built from CLASes through NVAPI instead - all of them in two
   operations after the classic builds, the BLAS at the address the plan gave
   it, as a classic one is. A plan the driver gives no sizes for builds them
   whole (the flag cleared).

   PERSISTENT (one per slot - the blood, pt_blood_blas_reuse): in memory of its
   own that the next frame on this slot does not lay anything over, so with
   `reuse` it may stand unbuilt - when that memory holds a BLAS built from this
   very geometry (vertex address, counts) with these flags. The caller vouches
   that the vertices are unchanged since (blood.c's BLAS REUSE); accel.c only
   that the BLAS is there. */
typedef struct {
    d3d12_as_triangles_t        geometry;       // nothing in it (no vertices, indices or boxes) = no BLAS
    bool                        clustered;      // in/out: from CLASes (pt_clusters)
    bool                        persistent;     // in: in the slot's persistent memory (the first one marked)
    bool                        reuse;          // in: may stand unbuilt (see PERSISTENT)
    bool                        reused;         // out: it did - nothing recorded for it
    D3D12_GPU_VIRTUAL_ADDRESS   address;        // out: the plan's, 0 = none
    uint64_t                    result_size;    // out
    uint64_t                    scratch_offset; // out
} d3d12_dynamic_blas_t;

typedef struct {
    d3d12_as_pool_t     pool;
    ID3D12Resource      *scratch;               // DEFAULT, UAV, created in COMMON
    uint64_t            scratch_size;

    // pt_clusters, this slot's: the CLASes (AS memory, referenced by the BLASes
    // built from them), their addresses (step 1 writes, step 2 reads), step 2's
    // destinations and result sizes (UAV: copied from the staging), the staging
    // (UPLOAD, mapped); and this frame's plan
    d3d12_as_pool_t     clas;
    ID3D12Resource      *clas_addresses;
    uint64_t            clas_addresses_size;
    ID3D12Resource      *blas_destinations;
    ID3D12Resource      *cluster_staging;
    uint8_t             *cluster_staging_map;
    uint64_t            cluster_staging_size;
    pt_cluster_plan_t   cluster_plan;           // num_blases 0: nothing clustered this frame
    pt_cluster_layout_t cluster_layout;
    d3d12_nvapi_cluster_inputs_t clas_inputs, blas_inputs;
    uint64_t            cluster_scratch_offset;

    // the persistent BLAS's memory, and what it was last built from (persist_size
    // 0: nothing to trust - set only when a build of it is planned)
    d3d12_as_pool_t     persist;
    d3d12_as_triangles_t persist_geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS persist_flags;
    uint64_t            persist_size;
} d3d12_dynamic_as_t;

// Sizes the builds and lays them out in `mem`, growing its pool and scratch
// when they do not fit - so the GPU must be past this memory's last use (the
// frame slot's previous frame). False: out of memory, and every address is 0.
bool d3d12_dynamic_blas_plan(ID3D12Device5 *device, d3d12_dynamic_as_t *mem, d3d12_dynamic_blas_t *blases,
                             int count, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags);
// Records the planned builds - their inputs must be build inputs
// (NON_PIXEL_SHADER_RESOURCE) by then - and a barrier after them, so a TLAS
// build can read them. False: NVAPI refused the clustered builds (nothing of
// them recorded, and their BLASes' memory holds whatever it held).
bool d3d12_dynamic_blas_build(ID3D12GraphicsCommandList4 *cmd, const d3d12_dynamic_as_t *mem,
                              const d3d12_dynamic_blas_t *blases, int count,
                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags);
// A run of a triangle-list positions buffer: num_prims triangles (9 floats
// each) from triangle first_prim
d3d12_as_triangles_t d3d12_triangle_run(D3D12_GPU_VIRTUAL_ADDRESS positions, uint32_t first_prim, uint32_t num_prims);
void d3d12_dynamic_as_destroy(d3d12_dynamic_as_t *mem);

/* ------------------------------------------------------------------------- */
/* THE PARTITIONED TLAS                                                       */
/* ------------------------------------------------------------------------- */

/* vkpt/ptlas.h's persistent, partitioned geometry TLAS - its core is API-free
   and the Vulkan renderer's too - on one of two D3D12 routes:
     NVAPI      NVIDIA's extension on the retail runtime (nvapi_rt.c): any GPU
                and driver that report it, no Windows setting
     PREVIEW    D3D12's own RT tier 2.0: the preview runtime only
                (d3d12_agility_preview 1, which needs Windows Developer Mode)
   Both read the same op list; the preview orders an argument's fields its own
   way (ptlas_stage converts). It is traced like any TLAS: an AS SRV is an
   address, so the descriptor does not care which kind is there. */
#include "../vkpt/ptlas.h"

typedef enum {
    D3D12_PTLAS_NONE,
    D3D12_PTLAS_NVAPI,
    D3D12_PTLAS_PREVIEW,
} d3d12_ptlas_route_t;

// the preview runtime's own API when the device reports tier 2.0, else NVAPI's,
// else none
d3d12_ptlas_route_t d3d12_ptlas_route(ID3D12Device5 *device);
const char *d3d12_ptlas_route_name(d3d12_ptlas_route_t route);
// ptlas_diff's build flags (both routes: FAST_TRACE 1, FAST_BUILD 2)
uint32_t d3d12_ptlas_flags(bool fast_trace);
// what a PTLAS of p's shape needs (update_scratch = scratch: updates are in place)
bool d3d12_ptlas_sizes(ID3D12Device5 *device, d3d12_ptlas_route_t route, const ptlas_t *p, bool fast_trace,
                       d3d12_as_sizes_t *sizes);
/* After ptlas_diff returned true: stages p's ops in `staging` (CPU-mapped, its
   GPU address `staging_gpu`, ptlas_stage_size bytes, untouched until the GPU is
   past this list) and records the build, or the update in place, of the PTLAS
   at `dest` (AS memory; `scratch` a UAV). A d3d12_as_barrier goes after it.
   False: the route refused, and the PTLAS starts from nothing next time. */
bool d3d12_ptlas_record(ID3D12GraphicsCommandList4 *cmd, d3d12_ptlas_route_t route, ptlas_t *p, bool fast_trace,
                        void *staging, D3D12_GPU_VIRTUAL_ADDRESS staging_gpu,
                        D3D12_GPU_VIRTUAL_ADDRESS dest, D3D12_GPU_VIRTUAL_ADDRESS scratch);

#endif // REFRESH_D3D12_ACCEL_H
