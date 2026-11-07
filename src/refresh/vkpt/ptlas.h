/*
THE PARTITIONED TLAS, API-free (both renderers)

A partitioned TLAS (NVIDIA "RTX Mega Geometry": VK_NV_partitioned_acceleration_
structure on Vulkan, NVAPI's NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect
on D3D12, D3D12's own RT tier 2.0 on the preview runtime) is a drop-in for the
per-frame geometry TLAS. It is traced exactly like a TLAS - same descriptor,
same intrinsics - and each instance keeps the index it has in vkpt's instance
list, so InstanceIndex() and InstanceID() (which index tlas_instance_prim_
offsets[] and the model tables) mean what they mean now. What changes is the
build:

  * it is PERSISTENT: built once from nothing (every instance absent), then
    updated in place (source == dest) with WRITE_INSTANCE ops for only the
    instances that changed since the last build. ptlas_diff finds them against
    a CPU copy of what the PTLAS holds. An index that falls out of use is
    written with a null BLAS, which makes it inactive.

  * it is PARTITIONED: the first `num_global` instances of the list - the
    static world, which vkpt_prepare_frame puts first and which spans the map -
    go to the global partition; the rest go to fixed-size chunks by index, so a
    moving entity rebuilds its chunk, not the tree.

  * the instances from `first_volatile` on are written EVERY build: their
    BLASes (vkpt's per-frame dynamic ones: the animated models, blood,
    viewer weapon) are rebuilt every frame, often at the same address, and a
    PTLAS keeps each instance's bounds from when it was written.

  * a STATIC BLAS rebuilt in place (a map load, a model registration) changes
    nothing an instance holds, so ptlas_static_changed() makes every PTLAS
    start again from nothing.

The op list lives in GPU-readable memory (an upload heap is fine): a uint32
count, the op descriptors, then the WRITE_INSTANCE arguments
(ptlas_stage_size bytes, see ptlas_stage).

NVAPI's and Vulkan's argument and op structs have the same layout
(ptlas_write_t, ptlas_op_t; each backend checks it against its own header).
D3D12's preview API orders the fields differently: ptlas_to_preview.

No engine headers: the headless tests compile ptlas.c on its own.
*/

#ifndef VKPT_PTLAS_H
#define VKPT_PTLAS_H

#include <stdint.h>
#include <stdbool.h>

#define PTLAS_INSTANCES_PER_PARTITION   32
#define PTLAS_PARTITION_GLOBAL          0xffffffffu
#define PTLAS_OP_WRITE_INSTANCE         0
// the global partition's capacity: vkpt's static world is 5 BLASes (opaque,
// transparent, masked, sky, custom sky)
#define PTLAS_MAX_GLOBAL                16

// QvkGeometryInstance_t / VkAccelerationStructureInstanceKHR /
// D3D12_RAYTRACING_INSTANCE_DESC: 3x4 row-major transform, 24-bit id + 8-bit
// mask, 24-bit SBT offset + 8-bit flags, the BLAS address (0 = none)
typedef struct {
    float       transform[12];
    uint32_t    id_mask;
    uint32_t    sbt_flags;
    uint64_t    blas;
} ptlas_instance_t;

// NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OP_ARG_WRITE_INSTANCE /
// VkPartitionedAccelerationStructureWriteInstanceDataNV
typedef struct {
    float       transform[12];
    float       aabb[6];            // explicit bounds: unused (the flag is never set)
    uint32_t    instance_id;
    uint32_t    instance_mask;
    uint32_t    sbt_offset;
    uint32_t    flags;              // cull disable 1, CCW/flip 2, opaque 4, non-opaque 8
    uint32_t    instance_index;
    uint32_t    partition_index;
    uint64_t    blas;               // 0: the instance is inactive
} ptlas_write_t;

// NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OP /
// VkBuildPartitionedAccelerationStructureIndirectCommandNV /
// D3D12_RTAS_PARTITIONED_TLAS_OPERATION
typedef struct {
    uint32_t    type;
    uint32_t    count;
    uint64_t    address;
    uint64_t    stride;
} ptlas_op_t;

typedef struct {
    uint32_t            max_instances;      // instance indices 0 .. max_instances - 1
    uint32_t            max_global;         // of them, at most this many in the global partition
    ptlas_instance_t    *held;              // what the PTLAS holds, per index (blas 0 = absent)
    uint32_t            held_count;         // indices >= this are known absent
    uint32_t            held_global;        // the num_global the held copy was written with
    uint32_t            generation;         // ptlas_static_changed's count at the last build
    uint32_t            build_flags;        // the backend's flags at the last build
    bool                built;              // false: the next build starts from nothing
    ptlas_write_t       *writes;            // this build's ops (max_instances)
    uint32_t            num_writes;
} ptlas_t;

bool ptlas_init(ptlas_t *p, uint32_t max_instances, uint32_t max_global);
void ptlas_shutdown(ptlas_t *p);
// the next build starts from nothing (its memory was overwritten, a device change)
void ptlas_invalidate(ptlas_t *p);
// a static BLAS was rebuilt, maybe in place: every PTLAS starts from nothing
void ptlas_static_changed(void);

// the partition count the build inputs give (indices >= num_global, in chunks)
uint32_t ptlas_partition_count(const ptlas_t *p);

/* This build's ops: the instance list against what the PTLAS holds - a
   WRITE_INSTANCE for every instance that is new, differs, or sits at or past
   first_volatile, and an inactive write for every index past `count` that was
   live. `build_flags`: the backend's flags (fast trace / fast build); a change
   starts from nothing. `instances` are ptlas_instance_t-shaped (64 bytes).
   Fills p->writes / p->num_writes and updates the held copy. Returns whether a
   build must be recorded (false: a built PTLAS and nothing to write). */
bool ptlas_diff(ptlas_t *p, const void *instances, uint32_t count, uint32_t num_global,
                uint32_t first_volatile, uint32_t build_flags);
// after ptlas_diff returned true: the build reads the PTLAS itself (an update
// in place) or nothing (a build from nothing)
bool ptlas_diff_is_update(const ptlas_t *p);

/* The staging block: a uint32 count at 0, one ptlas_op_t at PTLAS_STAGE_OP,
   the arguments at PTLAS_STAGE_ARGS (8-byte aligned, ptlas_write_t stride). */
#define PTLAS_STAGE_OP      16
#define PTLAS_STAGE_ARGS    64
uint64_t ptlas_stage_size(uint32_t max_instances);
/* Writes p->writes' ops into `staging` (CPU-mapped; its GPU address is
   `staging_gpu`): the count, the op, the arguments - in the preview API's
   field order when `preview`. Marks the PTLAS built. */
void ptlas_stage(ptlas_t *p, void *staging, uint64_t staging_gpu, bool preview);

#endif // VKPT_PTLAS_H
