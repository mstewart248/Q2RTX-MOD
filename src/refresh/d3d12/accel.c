/*
The acceleration structure layer, classic DXR backend. See accel.h.
*/

#include "accel.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* POOLS                                                                      */
/* ------------------------------------------------------------------------- */

bool d3d12_as_pool_create(d3d12_as_pool_t *pool, ID3D12Device *device, uint64_t size, const wchar_t *name)
{
    memset(pool, 0, sizeof(*pool));
    size = (size + D3D12_AS_ALIGNMENT - 1) & ~(uint64_t)(D3D12_AS_ALIGNMENT - 1);

    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // an acceleration structure buffer is created in this state and never leaves it
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, NULL,
                                                    &IID_ID3D12Resource, (void **)&pool->buffer)))
        return false;
    if (name)
        ID3D12Resource_SetName(pool->buffer, name);
    pool->size = size;
    return true;
}

void d3d12_as_pool_destroy(d3d12_as_pool_t *pool)
{
    if (pool->buffer)
        ID3D12Resource_Release(pool->buffer);
    memset(pool, 0, sizeof(*pool));
}

void d3d12_as_pool_reset(d3d12_as_pool_t *pool)
{
    pool->used = 0;
}

D3D12_GPU_VIRTUAL_ADDRESS d3d12_as_pool_alloc(d3d12_as_pool_t *pool, uint64_t size)
{
    if (!pool->buffer || !size)
        return 0;
    const uint64_t offset = (pool->used + D3D12_AS_ALIGNMENT - 1) & ~(uint64_t)(D3D12_AS_ALIGNMENT - 1);
    if (offset + size > pool->size)
        return 0;
    pool->used = offset + size;
    return ID3D12Resource_GetGPUVirtualAddress(pool->buffer) + offset;
}

/* ------------------------------------------------------------------------- */
/* BUILDS                                                                     */
/* ------------------------------------------------------------------------- */

/* A BLAS takes any number of geometries. vkpt gives a static model one per mesh
   (vkpt_append_model_geometry): an md3 has up to MD3_MAX_MESHES (32), and the
   rerelease shotgun pickups have 15 and 10. The hit shaders find a mesh by its
   GeometryIndex, so none may be dropped - a cap of 8 here once drew the pickups
   with half their surfaces. This many descs go on the stack, more on the heap. */
#define STACK_GEOMETRIES    32

static D3D12_RAYTRACING_GEOMETRY_DESC *geometry_descs(D3D12_RAYTRACING_GEOMETRY_DESC *stack, int n)
{
    return n <= STACK_GEOMETRIES ? stack : malloc(sizeof(*stack) * (size_t)n);
}

static void free_geometry_descs(D3D12_RAYTRACING_GEOMETRY_DESC *descs, D3D12_RAYTRACING_GEOMETRY_DESC *stack)
{
    if (descs != stack)
        free(descs);
}

static int fill_geometry(D3D12_RAYTRACING_GEOMETRY_DESC *out, const d3d12_as_triangles_t *in, int n)
{
    for (int i = 0; i < n; i++) {
        D3D12_RAYTRACING_GEOMETRY_DESC *g = &out[i];
        memset(g, 0, sizeof(*g));
        if (in[i].aabbs) {
            // vkpt's AABB geometry has no flags either: not opaque, so a ray
            // query sees each box as a procedural candidate
            g->Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            g->Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            g->AABBs.AABBCount = in[i].num_aabbs;
            g->AABBs.AABBs.StartAddress = in[i].aabbs;
            g->AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
            continue;
        }
        g->Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g->Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;    // as vkpt: the instance says what is opaque
        g->Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g->Triangles.VertexBuffer.StartAddress = in[i].vertices;
        g->Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
        g->Triangles.VertexCount = in[i].num_vertices;
        if (in[i].indices) {
            g->Triangles.IndexFormat = in[i].indices_32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            g->Triangles.IndexBuffer = in[i].indices;
            g->Triangles.IndexCount = in[i].num_indices;
        } else {
            g->Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
        }
    }
    return n;
}

static void fill_blas_inputs(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *in,
                             D3D12_RAYTRACING_GEOMETRY_DESC *geoms, const d3d12_as_triangles_t *geometry, int n,
                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags)
{
    memset(in, 0, sizeof(*in));
    in->Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in->Flags = flags;
    in->NumDescs = (UINT)fill_geometry(geoms, geometry, n);
    in->DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in->pGeometryDescs = geoms;
}

bool d3d12_blas_sizes(ID3D12Device5 *device, const d3d12_as_triangles_t *geometry, int num_geometries,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, d3d12_as_sizes_t *sizes)
{
    D3D12_RAYTRACING_GEOMETRY_DESC stack[STACK_GEOMETRIES];
    D3D12_RAYTRACING_GEOMETRY_DESC *geoms = geometry_descs(stack, num_geometries);
    if (!geoms)
        return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in;
    fill_blas_inputs(&in, geoms, geometry, num_geometries, flags);

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info;
    memset(&info, 0, sizeof(info));
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(device, &in, &info);
    free_geometry_descs(geoms, stack);
    sizes->result = info.ResultDataMaxSizeInBytes;
    sizes->scratch = info.ScratchDataSizeInBytes;
    sizes->update_scratch = info.UpdateScratchDataSizeInBytes;
    return info.ResultDataMaxSizeInBytes != 0;
}

void d3d12_blas_build(ID3D12GraphicsCommandList4 *cmd, const d3d12_as_triangles_t *geometry, int num_geometries,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, D3D12_GPU_VIRTUAL_ADDRESS dest,
                      D3D12_GPU_VIRTUAL_ADDRESS scratch, D3D12_GPU_VIRTUAL_ADDRESS source)
{
    // (a failed allocation builds nothing: the BLAS stays empty, every ray misses it)
    D3D12_RAYTRACING_GEOMETRY_DESC stack[STACK_GEOMETRIES];
    D3D12_RAYTRACING_GEOMETRY_DESC *geoms = geometry_descs(stack, num_geometries);
    if (!geoms)
        return;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    fill_blas_inputs(&desc.Inputs, geoms, geometry, num_geometries, flags);
    desc.DestAccelerationStructureData = dest;
    desc.ScratchAccelerationStructureData = scratch;
    desc.SourceAccelerationStructureData = source;
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cmd, &desc, 0, NULL);
    free_geometry_descs(geoms, stack);
}

static void fill_tlas_inputs(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *in, D3D12_GPU_VIRTUAL_ADDRESS instances,
                             uint32_t num_instances, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags)
{
    memset(in, 0, sizeof(*in));
    in->Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in->Flags = flags;
    in->NumDescs = num_instances;
    in->DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in->InstanceDescs = instances;
}

bool d3d12_tlas_sizes(ID3D12Device5 *device, uint32_t num_instances,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, d3d12_as_sizes_t *sizes)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in;
    fill_tlas_inputs(&in, 0, num_instances, flags);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info;
    memset(&info, 0, sizeof(info));
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(device, &in, &info);
    sizes->result = info.ResultDataMaxSizeInBytes;
    sizes->scratch = info.ScratchDataSizeInBytes;
    sizes->update_scratch = info.UpdateScratchDataSizeInBytes;
    return info.ResultDataMaxSizeInBytes != 0;
}

void d3d12_tlas_build(ID3D12GraphicsCommandList4 *cmd, D3D12_GPU_VIRTUAL_ADDRESS instances, uint32_t num_instances,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, D3D12_GPU_VIRTUAL_ADDRESS dest,
                      D3D12_GPU_VIRTUAL_ADDRESS scratch)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    fill_tlas_inputs(&desc.Inputs, instances, num_instances, flags);
    desc.DestAccelerationStructureData = dest;
    desc.ScratchAccelerationStructureData = scratch;
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cmd, &desc, 0, NULL);
}

/* ------------------------------------------------------------------------- */
/* THE PARTITIONED TLAS                                                       */
/* ------------------------------------------------------------------------- */

d3d12_ptlas_route_t d3d12_ptlas_route(ID3D12Device5 *device)
{
#ifdef D3D12_PREVIEW_SDK_VERSION
    // the retail runtime refuses the query
    D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2 rt2;
    memset(&rt2, 0, sizeof(rt2));
    if (SUCCEEDED(ID3D12Device5_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2, &rt2,
                                                    sizeof(rt2))) && rt2.ClustersAndPTLASSupported) {
        ID3D12DeviceRaytracing2 *rt = NULL;
        if (SUCCEEDED(ID3D12Device5_QueryInterface(device, &IID_ID3D12DeviceRaytracing2, (void **)&rt))) {
            ID3D12DeviceRaytracing2_Release(rt);
            return D3D12_PTLAS_PREVIEW;
        }
    }
#endif
    if (d3d12_nvapi_ptlas_supported((ID3D12Device *)device))
        return D3D12_PTLAS_NVAPI;
    return D3D12_PTLAS_NONE;
}

const char *d3d12_ptlas_route_name(d3d12_ptlas_route_t route)
{
    switch (route) {
    case D3D12_PTLAS_NVAPI:     return "NVAPI";
    case D3D12_PTLAS_PREVIEW:   return "D3D12 preview runtime";
    default:                    return "none";
    }
}

uint32_t d3d12_ptlas_flags(bool fast_trace)
{
    return fast_trace ? D3D12_NVAPI_PTLAS_FAST_TRACE : D3D12_NVAPI_PTLAS_FAST_BUILD;
}

static void nvapi_inputs(const ptlas_t *p, bool fast_trace, d3d12_nvapi_ptlas_inputs_t *in)
{
    memset(in, 0, sizeof(*in));
    in->flags = d3d12_ptlas_flags(fast_trace);
    in->instance_count = p->max_instances;
    in->max_instances_per_partition = PTLAS_INSTANCES_PER_PARTITION;
    in->partition_count = ptlas_partition_count(p);
    in->max_instances_in_global = p->max_global;
}

#ifdef D3D12_PREVIEW_SDK_VERSION
// the same flag values and op struct as NVAPI's
typedef char ptlas_preview_flags_check[(D3D12_RTAS_PARTITIONED_TLAS_FLAG_FAST_TRACE == D3D12_NVAPI_PTLAS_FAST_TRACE &&
                                        D3D12_RTAS_PARTITIONED_TLAS_FLAG_FAST_BUILD == D3D12_NVAPI_PTLAS_FAST_BUILD) ? 1 : -1];
typedef char ptlas_preview_op_check[sizeof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION) == sizeof(ptlas_op_t) ? 1 : -1];
typedef char ptlas_preview_args_check[sizeof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS) ==
                                      sizeof(ptlas_write_t) ? 1 : -1];

static void preview_inputs(const ptlas_t *p, bool fast_trace, D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC *in)
{
    memset(in, 0, sizeof(*in));
    in->Flags = (D3D12_RTAS_PARTITIONED_TLAS_FLAGS)d3d12_ptlas_flags(fast_trace);
    in->InstanceCount = p->max_instances;
    in->MaxInstancePerPartitionCount = PTLAS_INSTANCES_PER_PARTITION;
    in->PartitionCount = ptlas_partition_count(p);
    in->MaxInstanceInGlobalPartitionCount = p->max_global;
}
#endif

bool d3d12_ptlas_sizes(ID3D12Device5 *device, d3d12_ptlas_route_t route, const ptlas_t *p, bool fast_trace,
                       d3d12_as_sizes_t *sizes)
{
    memset(sizes, 0, sizeof(*sizes));
    if (route == D3D12_PTLAS_NVAPI) {
        d3d12_nvapi_ptlas_inputs_t in;
        nvapi_inputs(p, fast_trace, &in);
        if (!d3d12_nvapi_ptlas_sizes(device, &in, &sizes->result, &sizes->scratch))
            return false;
        sizes->update_scratch = sizes->scratch;
        return true;
    }
#ifdef D3D12_PREVIEW_SDK_VERSION
    if (route == D3D12_PTLAS_PREVIEW) {
        ID3D12DeviceRaytracing2 *rt = NULL;
        if (FAILED(ID3D12Device5_QueryInterface(device, &IID_ID3D12DeviceRaytracing2, (void **)&rt)))
            return false;
        D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC in;
        preview_inputs(p, fast_trace, &in);
        D3D12_RTAS_OPERATION_INPUTS op;
        memset(&op, 0, sizeof(op));
        op.Type = D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS;
        op.pPartitionedTLASInputsDesc = &in;
        D3D12_RTAS_OPERATION_PREBUILD_INFO info;
        memset(&info, 0, sizeof(info));
        ID3D12DeviceRaytracing2_GetRTASOperationPrebuildInfo(rt, &op, &info);
        ID3D12DeviceRaytracing2_Release(rt);
        sizes->result = info.ResultDataMaxSizeInBytes;
        sizes->scratch = info.ScratchDataSizeInBytes;
        sizes->update_scratch = info.ScratchDataSizeInBytes;
        return info.ResultDataMaxSizeInBytes != 0;
    }
#endif
    return false;
}

bool d3d12_ptlas_record(ID3D12GraphicsCommandList4 *cmd, d3d12_ptlas_route_t route, ptlas_t *p, bool fast_trace,
                        void *staging, D3D12_GPU_VIRTUAL_ADDRESS staging_gpu,
                        D3D12_GPU_VIRTUAL_ADDRESS dest, D3D12_GPU_VIRTUAL_ADDRESS scratch)
{
    // read before ptlas_stage marks it built: an update reads the PTLAS itself
    const D3D12_GPU_VIRTUAL_ADDRESS source = ptlas_diff_is_update(p) ? dest : 0;
    ptlas_stage(p, staging, staging_gpu, route == D3D12_PTLAS_PREVIEW);

    bool ok = false;
    if (route == D3D12_PTLAS_NVAPI) {
        d3d12_nvapi_ptlas_inputs_t in;
        nvapi_inputs(p, fast_trace, &in);
        ok = d3d12_nvapi_ptlas_build(cmd, &in, source, dest, scratch, staging_gpu, staging_gpu + PTLAS_STAGE_OP);
    }
#ifdef D3D12_PREVIEW_SDK_VERSION
    else if (route == D3D12_PTLAS_PREVIEW) {
        ID3D12CommandListRaytracing2 *rt = NULL;
        if (SUCCEEDED(ID3D12GraphicsCommandList4_QueryInterface(cmd, &IID_ID3D12CommandListRaytracing2, (void **)&rt))) {
            D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC in;
            preview_inputs(p, fast_trace, &in);
            D3D12_RTAS_PARTITIONED_TLAS_OPERATION_DATA data;
            memset(&data, 0, sizeof(data));
            data.SourceAccelerationStructureData = source;
            data.DestAccelerationStructureData = dest;
            data.ScratchAccelerationStructureData = scratch;
            data.IndirectPartitionedTlasOpCount = staging_gpu;
            data.IndirectPartitionedTlasOps = staging_gpu + PTLAS_STAGE_OP;
            D3D12_RTAS_OPERATION_DESC desc;
            memset(&desc, 0, sizeof(desc));
            desc.Inputs.Type = D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS;
            desc.Inputs.pPartitionedTLASInputsDesc = &in;
            desc.pPartitionedTlasOperationData = &data;
            ID3D12CommandListRaytracing2_ExecuteIndirectRTASOperations(rt, 1, &desc,
                                                                       D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
            ID3D12CommandListRaytracing2_Release(rt);
            ok = true;
        }
    }
#endif
    if (!ok)
        ptlas_invalidate(p);
    return ok;
}

void d3d12_as_barrier(ID3D12GraphicsCommandList4 *cmd, const d3d12_as_pool_t *pool)
{
    // DXR orders acceleration structure reads after writes with a UAV barrier
    // on the buffer that holds them
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = pool ? pool->buffer : NULL;
    ID3D12GraphicsCommandList4_ResourceBarrier(cmd, 1, &b);
}

/* ------------------------------------------------------------------------- */
/* THE PER-FRAME BLASes                                                       */
/* ------------------------------------------------------------------------- */

#define AS_ALIGN(x) (((x) + D3D12_AS_ALIGNMENT - 1) & ~(uint64_t)(D3D12_AS_ALIGNMENT - 1))

static ID3D12Resource *scratch_buffer(ID3D12Device *device, uint64_t size, const wchar_t *name)
{
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r = NULL;
    // COMMON: a build promotes it, and it decays back between command lists
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                    NULL, &IID_ID3D12Resource, (void **)&r)))
        return NULL;
    if (name)
        ID3D12Resource_SetName(r, name);
    return r;
}

// half as much again, in whole megabytes: a scene that grows by an entity
// should not reallocate every frame
static uint64_t grown(uint64_t needed)
{
    const uint64_t mb = 1ull << 20;
    return (needed + needed / 2 + mb - 1) & ~(mb - 1);
}

d3d12_as_triangles_t d3d12_triangle_run(D3D12_GPU_VIRTUAL_ADDRESS positions, uint32_t first_prim, uint32_t num_prims)
{
    d3d12_as_triangles_t g;
    memset(&g, 0, sizeof(g));
    if (num_prims) {
        g.vertices = positions + (uint64_t)first_prim * 9 * sizeof(float);
        g.num_vertices = num_prims * 3;
    }
    return g;
}

// nothing to build: no boxes, no vertices, or an index list with no indices
static bool geometry_empty(const d3d12_as_triangles_t *g)
{
    if (g->aabbs)
        return g->num_aabbs == 0;
    return !g->vertices || !g->num_vertices || (g->indices && !g->num_indices);
}

/* ---- the clustered ones (pt_clusters) ---- */

static ID3D12Resource *upload_buffer(ID3D12Device *device, uint64_t size, const wchar_t *name, uint8_t **map)
{
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
                                                    (void **)&r)))
        return NULL;
    D3D12_RANGE none = { 0, 0 };
    void *p = NULL;
    if (FAILED(ID3D12Resource_Map(r, 0, &none, &p))) {
        ID3D12Resource_Release(r);
        return NULL;
    }
    if (name)
        ID3D12Resource_SetName(r, name);
    *map = p;
    return r;
}

// step 2's destinations (a uint64 per BLAS), then its result sizes (a uint32 each)
#define BLAS_DESTINATIONS_SIZE  (PT_CLUSTER_MAX_BLASES * (sizeof(uint64_t) + sizeof(uint32_t)))
#define BLAS_RESULT_SIZES       (PT_CLUSTER_MAX_BLASES * sizeof(uint64_t))

static void unclustered(d3d12_dynamic_as_t *mem, d3d12_dynamic_blas_t *blases, int count)
{
    for (int i = 0; i < count; i++)
        blases[i].clustered = false;
    memset(&mem->cluster_plan, 0, sizeof(mem->cluster_plan));
}

/* The clustered BLASes' plan: sizes from NVAPI for step 1 (all CLASes, implicit
   destinations) and step 2 (the BLASes, explicit), each BLAS's own result size.
   Anything the driver will not size is built whole. Returns the scratch both
   steps need (one after the other: they share it), 0 = nothing clustered; the
   CLAS memory needed goes to *clas_size. */
static uint64_t cluster_plan(ID3D12Device5 *device, d3d12_dynamic_as_t *mem, d3d12_dynamic_blas_t *blases, int count,
                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, uint64_t *clas_size)
{
    *clas_size = 0;
    memset(&mem->cluster_plan, 0, sizeof(mem->cluster_plan));
    uint32_t prims[PT_CLUSTER_MAX_BLASES];
    uint32_t num_runs = 0;
    for (int i = 0; i < count; i++) {
        d3d12_dynamic_blas_t *b = &blases[i];
        if (!b->clustered)
            continue;
        // a plain triangle list, and a run slot for it
        if (geometry_empty(&b->geometry) || b->geometry.indices || b->geometry.aabbs || num_runs == PT_CLUSTER_MAX_BLASES) {
            b->clustered = false;
            continue;
        }
        prims[num_runs++] = b->geometry.num_vertices / 3;
    }
    if (!num_runs)
        return 0;
    pt_cluster_plan_t *plan = &mem->cluster_plan;
    if (!pt_cluster_plan(plan, prims, num_runs)) {
        unclustered(mem, blases, count);
        return 0;
    }

    const uint32_t op_flags = (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE)
        ? D3D12_NVAPI_CLUSTER_FAST_TRACE : D3D12_NVAPI_CLUSTER_FAST_BUILD;

    d3d12_nvapi_cluster_inputs_t *c = &mem->clas_inputs;
    memset(c, 0, sizeof(*c));
    c->max_arg_count = plan->num_clusters;
    c->flags = op_flags;
    c->type = D3D12_NVAPI_CLUSTER_CLAS_FROM_TRIANGLES;
    c->mode = D3D12_NVAPI_CLUSTER_IMPLICIT;
    c->u.triangles.vertex_format = DXGI_FORMAT_R32G32B32_FLOAT;
    c->u.triangles.max_geometry_index = plan->max_clusters_per_blas - 1;
    c->u.triangles.max_unique_geometries_per_arg = 1;
    c->u.triangles.max_triangles_per_arg = PT_CLUSTER_TRIANGLES;
    c->u.triangles.max_vertices_per_arg = PT_CLUSTER_VERTICES;
    c->u.triangles.max_total_triangles = plan->num_triangles;
    c->u.triangles.max_total_vertices = 3 * plan->num_triangles;

    d3d12_nvapi_cluster_inputs_t *bl = &mem->blas_inputs;
    memset(bl, 0, sizeof(*bl));
    bl->max_arg_count = plan->num_blases;
    bl->flags = op_flags;
    bl->type = D3D12_NVAPI_CLUSTER_BLAS_FROM_CLAS;
    bl->mode = D3D12_NVAPI_CLUSTER_EXPLICIT;
    bl->u.clas.max_total_clas = plan->num_clusters;
    bl->u.clas.max_clas_per_arg = plan->max_clusters_per_blas;

    uint64_t clas_scratch = 0, blas_max = 0, blas_scratch = 0;
    bool ok = d3d12_nvapi_cluster_sizes(device, c, clas_size, &clas_scratch) &&
              d3d12_nvapi_cluster_sizes(device, bl, &blas_max, &blas_scratch);
    // each BLAS's own: the explicit size is the most one of these inputs makes
    uint32_t run = 0;
    for (int i = 0; ok && i < count; i++) {
        d3d12_dynamic_blas_t *b = &blases[i];
        if (!b->clustered)
            continue;
        d3d12_nvapi_cluster_inputs_t one = *bl;
        one.max_arg_count = 1;
        one.u.clas.max_total_clas = one.u.clas.max_clas_per_arg = plan->clusters[run++];
        uint64_t unused;
        ok = d3d12_nvapi_cluster_sizes(device, &one, &b->result_size, &unused);
    }
    if (!ok) {
        // (the caller sees the flags cleared and says so)
        for (int i = 0; i < count; i++)
            if (blases[i].clustered)
                blases[i].result_size = 0;
        unclustered(mem, blases, count);
        *clas_size = 0;
        return 0;
    }
    pt_cluster_layout(plan, &mem->cluster_layout);
    return AS_ALIGN(clas_scratch) > AS_ALIGN(blas_scratch) ? AS_ALIGN(clas_scratch) : AS_ALIGN(blas_scratch);
}

// the clustered plan's buffers, grown to fit (the GPU is past this slot's last use)
static bool cluster_buffers(ID3D12Device *device, d3d12_dynamic_as_t *mem, uint64_t clas_size)
{
    const pt_cluster_plan_t *plan = &mem->cluster_plan;
    if (clas_size > mem->clas.size) {
        d3d12_as_pool_destroy(&mem->clas);
        if (!d3d12_as_pool_create(&mem->clas, device, grown(clas_size), L"dynamic CLASes"))
            return false;
    }
    const uint64_t addresses = (uint64_t)plan->num_clusters * sizeof(uint64_t);
    if (addresses > mem->clas_addresses_size) {
        if (mem->clas_addresses)
            ID3D12Resource_Release(mem->clas_addresses);
        mem->clas_addresses_size = grown(addresses);
        mem->clas_addresses = scratch_buffer(device, mem->clas_addresses_size, L"dynamic CLAS addresses");
        if (!mem->clas_addresses) {
            mem->clas_addresses_size = 0;
            return false;
        }
    }
    if (!mem->blas_destinations &&
        !(mem->blas_destinations = scratch_buffer(device, AS_ALIGN(BLAS_DESTINATIONS_SIZE), L"clustered BLAS destinations")))
        return false;
    if (mem->cluster_layout.size > mem->cluster_staging_size) {
        if (mem->cluster_staging)
            ID3D12Resource_Release(mem->cluster_staging);
        mem->cluster_staging_size = grown(mem->cluster_layout.size);
        mem->cluster_staging = upload_buffer(device, mem->cluster_staging_size, L"cluster staging",
                                             &mem->cluster_staging_map);
        if (!mem->cluster_staging) {
            mem->cluster_staging_size = 0;
            mem->cluster_staging_map = NULL;
            return false;
        }
    }
    return true;
}

// the persistent BLAS was built from exactly this (see PERSISTENT in accel.h)
static bool same_geometry(const d3d12_as_triangles_t *a, const d3d12_as_triangles_t *b)
{
    return a->vertices == b->vertices && a->num_vertices == b->num_vertices &&
           a->indices == b->indices && a->num_indices == b->num_indices &&
           a->aabbs == b->aabbs && a->num_aabbs == b->num_aabbs && a->indices_32 == b->indices_32;
}

bool d3d12_dynamic_blas_plan(ID3D12Device5 *device, d3d12_dynamic_as_t *mem, d3d12_dynamic_blas_t *blases,
                             int count, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags)
{
    for (int i = 0; i < count; i++) {
        blases[i].address = 0;
        blases[i].result_size = 0;
        blases[i].scratch_offset = 0;
        blases[i].reused = false;
    }
    uint64_t clas_size = 0;
    const uint64_t cluster_scratch = cluster_plan(device, mem, blases, count, flags, &clas_size);

    // persistent: the first one marked (and not clustered); persist_needed its new size
    int persistent = -1;
    uint64_t pool_size = 0, scratch_size = 0, persist_needed = 0;
    for (int i = 0; i < count; i++) {
        d3d12_dynamic_blas_t *b = &blases[i];
        if (b->clustered) {
            pool_size = AS_ALIGN(pool_size) + b->result_size;
            continue;
        }
        if (geometry_empty(&b->geometry))
            continue;
        if (b->persistent && persistent < 0) {
            persistent = i;
            if (b->reuse && mem->persist.buffer && mem->persist_size && mem->persist_flags == flags &&
                same_geometry(&mem->persist_geometry, &b->geometry)) {
                b->reused = true;   // no scratch, no build: where it already is
                b->result_size = mem->persist_size;
                continue;
            }
        }
        d3d12_as_sizes_t s;
        if (!d3d12_blas_sizes(device, &b->geometry, 1, flags, &s))
            continue;
        b->result_size = s.result;
        b->scratch_offset = scratch_size;
        scratch_size += AS_ALIGN(s.scratch);
        if (i == persistent)
            persist_needed = s.result;
        else
            pool_size = AS_ALIGN(pool_size) + s.result;
    }
    // the clustered steps' scratch past every classic build's: no barrier between them
    mem->cluster_scratch_offset = scratch_size;
    scratch_size += cluster_scratch;
    if (!pool_size && !persist_needed && !(persistent >= 0 && blases[persistent].reused))
        return true;

    // every scratch region apart: the builds need no barriers between them
    if (pool_size > mem->pool.size) {
        d3d12_as_pool_destroy(&mem->pool);
        if (!d3d12_as_pool_create(&mem->pool, (ID3D12Device *)device, grown(pool_size), L"dynamic BLASes"))
            return false;
    }
    if (scratch_size > mem->scratch_size) {
        if (mem->scratch)
            ID3D12Resource_Release(mem->scratch);
        mem->scratch_size = grown(scratch_size);
        mem->scratch = scratch_buffer((ID3D12Device *)device, mem->scratch_size, L"dynamic BLAS scratch");
        if (!mem->scratch) {
            mem->scratch_size = 0;
            return false;
        }
    }

    // the persistent one rebuilt: nothing in its memory to trust until it is (the GPU
    // is past this slot's last use, so the memory may be remade)
    if (persist_needed) {
        mem->persist_size = 0;
        if (persist_needed > mem->persist.size) {
            d3d12_as_pool_destroy(&mem->persist);
            if (!d3d12_as_pool_create(&mem->persist, (ID3D12Device *)device, grown(persist_needed),
                                      L"persistent BLAS (blood)"))
                return false;
        }
    }

    if (mem->cluster_plan.num_blases && !cluster_buffers((ID3D12Device *)device, mem, clas_size))
        return false;

    d3d12_as_pool_reset(&mem->pool);
    for (int i = 0; i < count; i++) {
        d3d12_dynamic_blas_t *b = &blases[i];
        if (!b->result_size)
            continue;
        if (i != persistent) {
            b->address = d3d12_as_pool_alloc(&mem->pool, b->result_size);
            continue;
        }
        // alone in its memory, at its start - where a reused one already stands
        d3d12_as_pool_reset(&mem->persist);
        b->address = d3d12_as_pool_alloc(&mem->persist, b->result_size);
        if (b->address && !b->reused) {
            mem->persist_geometry = b->geometry;
            mem->persist_flags = flags;
            mem->persist_size = b->result_size;
        }
    }

    // the clustered builds' arguments, now that their BLASes have addresses
    const pt_cluster_plan_t *plan = &mem->cluster_plan;
    if (plan->num_blases) {
        uint64_t vertices[PT_CLUSTER_MAX_BLASES], address[PT_CLUSTER_MAX_BLASES];
        uint32_t run = 0;
        for (int i = 0; i < count; i++)
            if (blases[i].clustered) {
                vertices[run] = blases[i].geometry.vertices;
                address[run++] = blases[i].address;
            }
        pt_cluster_stage(plan, &mem->cluster_layout, mem->cluster_staging_map,
                         ID3D12Resource_GetGPUVirtualAddress(mem->cluster_staging), vertices, address,
                         ID3D12Resource_GetGPUVirtualAddress(mem->clas_addresses), 0);
    }
    return true;
}

static D3D12_RESOURCE_BARRIER buffer_transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                                                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

/* The plan's two cluster operations. The address arrays are COMMON when a command
   list starts (buffers decay to it), and NVAPI wants step 2's destinations in a
   UAV - hence the copy from the staging. */
static bool record_clusters(ID3D12GraphicsCommandList4 *cmd, const d3d12_dynamic_as_t *mem,
                            D3D12_GPU_VIRTUAL_ADDRESS scratch)
{
    const pt_cluster_plan_t *plan = &mem->cluster_plan;
    const pt_cluster_layout_t *layout = &mem->cluster_layout;
    ID3D12GraphicsCommandList *gcmd = (ID3D12GraphicsCommandList *)cmd;
    const D3D12_GPU_VIRTUAL_ADDRESS staging = ID3D12Resource_GetGPUVirtualAddress(mem->cluster_staging);
    const D3D12_GPU_VIRTUAL_ADDRESS addresses = ID3D12Resource_GetGPUVirtualAddress(mem->clas_addresses);
    const D3D12_GPU_VIRTUAL_ADDRESS destinations = ID3D12Resource_GetGPUVirtualAddress(mem->blas_destinations);

    D3D12_RESOURCE_BARRIER b[2];
    b[0] = buffer_transition(mem->blas_destinations, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_ResourceBarrier(gcmd, 1, b);
    ID3D12GraphicsCommandList_CopyBufferRegion(gcmd, mem->blas_destinations, 0, mem->cluster_staging,
                                               layout->blas_addresses, (uint64_t)plan->num_blases * sizeof(uint64_t));
    b[0] = buffer_transition(mem->blas_destinations, D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    b[1] = buffer_transition(mem->clas_addresses, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12GraphicsCommandList_ResourceBarrier(gcmd, 2, b);

    // 1: every cluster's CLAS, packed into this slot's CLAS memory, their addresses written
    if (!d3d12_nvapi_cluster_execute(cmd, &mem->clas_inputs, mem->clas.buffer ? ID3D12Resource_GetGPUVirtualAddress(mem->clas.buffer) : 0,
                                     scratch, addresses, 0, staging + layout->triangle_args, sizeof(pt_cluster_args_t)))
        return false;

    // the CLASes and their addresses written and the scratch free again; the addresses an input now
    memset(&b[0], 0, sizeof(b[0]));
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b[1] = buffer_transition(mem->clas_addresses, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(gcmd, 2, b);

    // 2: each run's BLAS from its slice of those addresses, where the plan put it
    return d3d12_nvapi_cluster_execute(cmd, &mem->blas_inputs, 0, scratch, destinations,
                                       destinations + BLAS_RESULT_SIZES, staging + layout->blas_args,
                                       sizeof(pt_cluster_blas_args_t));
}

bool d3d12_dynamic_blas_build(ID3D12GraphicsCommandList4 *cmd, const d3d12_dynamic_as_t *mem,
                              const d3d12_dynamic_blas_t *blases, int count,
                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags)
{
    if (!mem->scratch)
        return true;
    const D3D12_GPU_VIRTUAL_ADDRESS scratch = ID3D12Resource_GetGPUVirtualAddress(mem->scratch);
    bool any = false, persist_built = false;
    for (int i = 0; i < count; i++) {
        const d3d12_dynamic_blas_t *b = &blases[i];
        if (!b->address || b->clustered || b->reused)
            continue;
        d3d12_blas_build(cmd, &b->geometry, 1, flags, b->address, scratch + b->scratch_offset, 0);
        any = true;
        persist_built |= b->persistent;
    }
    bool ok = true;
    if (mem->cluster_plan.num_blases && mem->cluster_staging) {
        ok = record_clusters(cmd, mem, scratch + mem->cluster_scratch_offset);
        any = true;
    }
    // the persistent one is in another buffer: every UAV access then
    if (any)
        d3d12_as_barrier(cmd, persist_built ? NULL : &mem->pool);
    return ok;
}

void d3d12_dynamic_as_destroy(d3d12_dynamic_as_t *mem)
{
    d3d12_as_pool_destroy(&mem->pool);
    d3d12_as_pool_destroy(&mem->persist);
    if (mem->scratch)
        ID3D12Resource_Release(mem->scratch);
    d3d12_as_pool_destroy(&mem->clas);
    if (mem->clas_addresses)
        ID3D12Resource_Release(mem->clas_addresses);
    if (mem->blas_destinations)
        ID3D12Resource_Release(mem->blas_destinations);
    if (mem->cluster_staging)
        ID3D12Resource_Release(mem->cluster_staging);
    memset(mem, 0, sizeof(*mem));
}
