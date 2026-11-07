/*
RAY TRACED SOUND for the D3D12 renderer (vid_rtx 3): vkpt/sound_trace.c's GPU half
(s_reflections_gpu, s_diffraction) on D3D12.

What a run is, what the scene is and what comes back are sound_trace.c's: the
checks, the scene's packing (vertices, indices, each triangle's normal and
material, the edges), the parameters and the unpacking are its functions
(vkpt/sound_trace.h), called, not copied. This file is the API half, as
sound_trace.c's Vulkan half is:
    a compute queue of its own, which only the sound's simulation thread submits
    to and waits on (qvk.queue_sound there);
    the scene: a BLAS per mesh with faces, all in one pool (accel.c, with 32-bit
    indices), and a TLAS with room for every mesh, rebuilt every run from where
    steamaudio.c placed them (D3D12_RAYTRACING_INSTANCE_DESC is
    VkAccelerationStructureInstanceKHR);
    sound_trace.comp and sound_diffract.comp against the "sound" set (bindings.c's
    build_sound: TLAS, params, tris, fields, edges, diffraction).

THREADS, as sound_trace.c: runs come on the sound's simulation thread, and
snd_lock (an SRW lock, never freed: the sound may ask after the renderer has
gone) keeps them apart from init and shutdown. Nothing on that thread prints or
touches the zone - the buffers here are made without D3D_CHECK - and its
descriptors are its own set's slots (device calls, which are free-threaded).
D3D12 has no vkDeviceWaitIdle rule, so the renderer's idle waits need no lock.

STATES: every buffer here decays to COMMON after each ExecuteCommandLists and is
promoted from it on first use, so only a change of write state within one list
(the fields: zeroed by a copy, then written by the shader, then copied out) needs
a barrier.
*/

#include "d3d12pt.h"
#include "../vkpt/sound_trace.h"

#define SND_WAIT_MS     2000
#define SND_ALIGN_UP(x, a)  (((x) + (uint64_t)(a) - 1) & ~((uint64_t)(a) - 1))

static const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS snd_build_flags =
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

static SRWLOCK snd_lock = SRWLOCK_INIT;

static struct {
    bool                        up;         // set up on this device; the rest under snd_lock
    const char                  *why;       // why there is no tracing, NULL = there is

    ID3D12CommandQueue          *queue;
    ID3D12CommandAllocator      *allocator;
    ID3D12GraphicsCommandList4  *cmd;
    ID3D12Fence                 *fence;
    uint64_t                    fence_value;
    HANDLE                      event;

    d3d12_set_t                 set;
    ID3D12PipelineState         *pipeline, *diff_pipeline;

    ID3D12Resource  *params;                // upload, mapped: the CBV
    void            *params_map;
    ID3D12Resource  *fields, *diff;         // what the shaders write
    ID3D12Resource  *zeros;                 // copied over the fields first (vkCmdFillBuffer)
    ID3D12Resource  *readback, *diff_readback;
    void            *readback_map, *diff_readback_map;

    // the scene, of the geometry with this key; 0 = none
    unsigned        key;
    unsigned        failed_key;             // one whose scene could not be built: not tried again
    int             num_meshes;
    D3D12_GPU_VIRTUAL_ADDRESS *blas;        // per mesh; 0 = no faces
    uint32_t        *first_tri;             // per mesh: its triangles' place in tris
    d3d12_as_pool_t blas_pool, tlas_pool;
    D3D12_GPU_VIRTUAL_ADDRESS tlas;
    ID3D12Resource  *tris, *edges;
    int             num_edges;
    ID3D12Resource  *instances;             // upload, mapped
    void            *instances_map;
    ID3D12Resource  *scratch;               // one for every build, as they go one at a time
} snd;

// No D3D_CHECK: on the simulation thread nothing may print. UPLOAD and READBACK
// buffers are mapped into *map when it is given.
static ID3D12Resource *snd_buffer(uint64_t size, D3D12_HEAP_TYPE heap, bool uav, void **map)
{
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = heap;

    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size ? size : 1;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (heap == D3D12_HEAP_TYPE_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;

    ID3D12Resource *res = NULL;
    if (FAILED(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                     &IID_ID3D12Resource, (void **)&res)))
        return NULL;
    if (map) {
        // an upload buffer is never read back; a readback one is read whole
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(ID3D12Resource_Map(res, 0, heap == D3D12_HEAP_TYPE_UPLOAD ? &none : NULL, map))) {
            ID3D12Resource_Release(res);
            return NULL;
        }
    }
    return res;
}

static void snd_transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList4_ResourceBarrier(snd.cmd, 1, &b);
}

static bool snd_begin(void)
{
    return SUCCEEDED(ID3D12CommandAllocator_Reset(snd.allocator)) &&
           SUCCEEDED(ID3D12GraphicsCommandList4_Reset(snd.cmd, snd.allocator, NULL));
}

// Waits for everything submitted. A lost device signals the fence to its end.
static void snd_wait_idle(void)
{
    if (!snd.queue || !snd.fence || !snd.event)
        return;
    if (FAILED(ID3D12CommandQueue_Signal(snd.queue, snd.fence, ++snd.fence_value)))
        return;
    if (ID3D12Fence_GetCompletedValue(snd.fence) < snd.fence_value &&
        SUCCEEDED(ID3D12Fence_SetEventOnCompletion(snd.fence, snd.fence_value, snd.event)))
        WaitForSingleObject(snd.event, INFINITE);
}

// Executes and waits. A GPU that has not finished in SND_WAIT_MS is hung or lost;
// nothing is submitted here again.
static bool snd_submit(void)
{
    if (FAILED(ID3D12GraphicsCommandList4_Close(snd.cmd)))
        return false;
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)snd.cmd };
    ID3D12CommandQueue_ExecuteCommandLists(snd.queue, 1, lists);
    if (FAILED(ID3D12CommandQueue_Signal(snd.queue, snd.fence, ++snd.fence_value))) {
        snd.why = "the GPU refused a run";
        return false;
    }
    if (ID3D12Fence_GetCompletedValue(snd.fence) < snd.fence_value &&
        (FAILED(ID3D12Fence_SetEventOnCompletion(snd.fence, snd.fence_value, snd.event)) ||
         WaitForSingleObject(snd.event, SND_WAIT_MS) != WAIT_OBJECT_0)) {
        snd.why = "the GPU did not finish a run";
        return false;
    }
    return true;
}

static void snd_free_scene(void)
{
    free(snd.blas);
    free(snd.first_tri);
    snd.blas = NULL;
    snd.first_tri = NULL;
    snd.num_meshes = 0;
    d3d12_as_pool_destroy(&snd.blas_pool);
    d3d12_as_pool_destroy(&snd.tlas_pool);
    snd.tlas = 0;
    D3D12_RELEASE(snd.instances);
    snd.instances_map = NULL;
    D3D12_RELEASE(snd.scratch);
    D3D12_RELEASE(snd.tris);
    D3D12_RELEASE(snd.edges);
    snd.num_edges = 0;
    snd.key = 0;
}

// a mesh's triangles, out of the build's input buffers
static d3d12_as_triangles_t snd_mesh_geometry(D3D12_GPU_VIRTUAL_ADDRESS verts, D3D12_GPU_VIRTUAL_ADDRESS indices,
                                              size_t first_vert, uint32_t first_tri, const r_sound_mesh_t *m)
{
    d3d12_as_triangles_t g;
    memset(&g, 0, sizeof(g));
    g.vertices = verts + (uint64_t)first_vert * 3 * sizeof(float);
    g.num_vertices = (uint32_t)m->num_verts;
    g.indices = indices + (uint64_t)first_tri * 3 * sizeof(uint32_t);
    g.num_indices = (uint32_t)m->num_tris * 3;
    g.indices_32 = true;
    return g;
}

/*
The scene of `geom` (sound_trace.c's snd_build_scene): its triangles' normals and
materials for the shader, its edges, a BLAS per mesh with faces, and a TLAS with
room for every mesh at once. The build's vertex and index buffers are dropped
once it is done.
*/
static bool snd_build_scene(const r_sound_geom_t *geom)
{
    ID3D12Resource  *verts = NULL, *indices = NULL, *staging = NULL, *edge_staging = NULL;
    void            *verts_map = NULL, *indices_map = NULL, *staging_map = NULL, *edge_map = NULL;
    size_t          *first_vert = NULL;
    uint64_t        *result_size = NULL;
    size_t          nv = 0, nt = 0;
    const size_t    edges_size = vkpt_sound_edges_size(geom);
    uint64_t        scratch = 0, blas_size = 0;
    d3d12_as_sizes_t sizes;
    bool            ok = false, submitted = false;
    int             i;

    snd_free_scene();

    if (!vkpt_sound_scene_size(geom, &nv, &nt))
        return false;

    snd.blas = calloc(geom->num_meshes, sizeof(*snd.blas));
    snd.first_tri = calloc(geom->num_meshes, sizeof(*snd.first_tri));
    first_vert = calloc(geom->num_meshes, sizeof(*first_vert));
    result_size = calloc(geom->num_meshes, sizeof(*result_size));
    if (!snd.blas || !snd.first_tri || !first_vert || !result_size)
        goto done;
    snd.num_meshes = geom->num_meshes;

    verts = snd_buffer(nv * 3 * sizeof(float), D3D12_HEAP_TYPE_UPLOAD, false, &verts_map);
    indices = snd_buffer(nt * 3 * sizeof(uint32_t), D3D12_HEAP_TYPE_UPLOAD, false, &indices_map);
    staging = snd_buffer(nt * sizeof(vkpt_sound_tri_t), D3D12_HEAP_TYPE_UPLOAD, false, &staging_map);
    edge_staging = snd_buffer(edges_size, D3D12_HEAP_TYPE_UPLOAD, false, &edge_map);
    snd.tris = snd_buffer(nt * sizeof(vkpt_sound_tri_t), D3D12_HEAP_TYPE_DEFAULT, true, NULL);
    snd.edges = snd_buffer(edges_size, D3D12_HEAP_TYPE_DEFAULT, true, NULL);
    if (!verts || !indices || !staging || !edge_staging || !snd.tris || !snd.edges)
        goto done;

    vkpt_sound_pack_edges(geom, edge_map);
    snd.num_edges = max(geom->num_edges, 0);
    if (!vkpt_sound_pack_scene(geom, verts_map, indices_map, staging_map, first_vert, snd.first_tri))
        goto done;

    const D3D12_GPU_VIRTUAL_ADDRESS verts_va = ID3D12Resource_GetGPUVirtualAddress(verts);
    const D3D12_GPU_VIRTUAL_ADDRESS indices_va = ID3D12Resource_GetGPUVirtualAddress(indices);

    // a BLAS per mesh with faces, all in one pool
    for (i = 0; i < geom->num_meshes; i++) {
        const r_sound_mesh_t *m = &geom->meshes[i];
        if (!m->num_tris)
            continue;
        const d3d12_as_triangles_t g = snd_mesh_geometry(verts_va, indices_va, first_vert[i], snd.first_tri[i], m);
        if (!d3d12_blas_sizes(d3d.device, &g, 1, snd_build_flags, &sizes))
            goto done;
        result_size[i] = sizes.result;
        blas_size += SND_ALIGN_UP(sizes.result, D3D12_AS_ALIGNMENT);
        scratch = max(scratch, sizes.scratch);
    }
    if (!d3d12_as_pool_create(&snd.blas_pool, (ID3D12Device *)d3d.device, max(blas_size, D3D12_AS_ALIGNMENT),
                              L"sound BLASes"))
        goto done;
    for (i = 0; i < geom->num_meshes; i++)
        if (geom->meshes[i].num_tris && !(snd.blas[i] = d3d12_as_pool_alloc(&snd.blas_pool, result_size[i])))
            goto done;

    // the TLAS, with room for them all
    if (!d3d12_tlas_sizes(d3d.device, (uint32_t)geom->num_meshes, snd_build_flags, &sizes) ||
        !d3d12_as_pool_create(&snd.tlas_pool, (ID3D12Device *)d3d.device, sizes.result, L"sound TLAS") ||
        !(snd.tlas = d3d12_as_pool_alloc(&snd.tlas_pool, sizes.result)))
        goto done;
    scratch = max(scratch, sizes.scratch);

    snd.instances = snd_buffer((uint64_t)geom->num_meshes * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                               D3D12_HEAP_TYPE_UPLOAD, false, &snd.instances_map);
    snd.scratch = snd_buffer(scratch, D3D12_HEAP_TYPE_DEFAULT, true, NULL);
    if (!snd.instances || !snd.scratch)
        goto done;

    if (!snd_begin())
        goto done;

    ID3D12GraphicsCommandList4_CopyBufferRegion(snd.cmd, snd.tris, 0, staging, 0, nt * sizeof(vkpt_sound_tri_t));
    ID3D12GraphicsCommandList4_CopyBufferRegion(snd.cmd, snd.edges, 0, edge_staging, 0, edges_size);

    const D3D12_GPU_VIRTUAL_ADDRESS scratch_va = ID3D12Resource_GetGPUVirtualAddress(snd.scratch);
    for (i = 0; i < geom->num_meshes; i++) {
        if (!snd.blas[i])
            continue;
        const d3d12_as_triangles_t g = snd_mesh_geometry(verts_va, indices_va, first_vert[i], snd.first_tri[i],
                                                         &geom->meshes[i]);
        d3d12_blas_build(snd.cmd, &g, 1, snd_build_flags, snd.blas[i], scratch_va, 0);
        // the next build reuses the scratch
        d3d12_as_barrier(snd.cmd, NULL);
    }

    submitted = true;
    if (!snd_submit())
        goto done;

    d3d12_set_write_as(&snd.set, 0, 0, snd.tlas);
    d3d12_set_write_buffer(&snd.set, 2, 0, snd.tris, 0, nt * sizeof(vkpt_sound_tri_t));
    d3d12_set_write_buffer(&snd.set, 4, 0, snd.edges, 0, edges_size);

    snd.key = geom->key;
    ok = true;

done:
    if (submitted && !ok)
        snd_wait_idle();
    D3D12_RELEASE(verts);
    D3D12_RELEASE(indices);
    D3D12_RELEASE(staging);
    D3D12_RELEASE(edge_staging);
    free(first_vert);
    free(result_size);
    if (!ok)
        snd_free_scene();
    return ok;
}

// NULL, or why the run was not made (sound_trace.c's snd_run)
static const char *snd_run(const r_sound_trace_t *t)
{
    uint32_t    field_floats = 0, groups = 0;
    int         i, n = 0;

    const char *why = vkpt_sound_check_run(t);
    if (why)
        return why;
    if (t->num_queries > 0 && !snd.diff_pipeline)
        return "no shader for it";

    if (snd.key != t->geom->key) {
        if (snd.failed_key == t->geom->key)
            return "couldn't build the map's scene";
        if (!snd_build_scene(t->geom)) {
            snd.failed_key = t->geom->key;
            return snd.why ? snd.why : "couldn't build the map's scene";
        }
    }

    // the meshes where steamaudio.c has placed them; built here and copied whole,
    // as the upload memory is write-combined
    D3D12_RAYTRACING_INSTANCE_DESC *inst = snd.instances_map;
    for (i = 0; i < t->num_instances && n < snd.num_meshes; i++) {
        const r_sound_instance_t *in = &t->instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC d;

        if (in->mesh < 0 || in->mesh >= snd.num_meshes || !snd.blas[in->mesh])
            continue;
        memset(&d, 0, sizeof(d));
        memcpy(d.Transform, in->transform, sizeof(d.Transform));
        d.InstanceID = snd.first_tri[in->mesh];
        d.InstanceMask = 0xff;
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        d.AccelerationStructure = snd.blas[in->mesh];
        memcpy(&inst[n], &d, sizeof(d));
        n++;
    }

    vkpt_sound_pack_params(t, snd.num_edges, snd.params_map, &groups, &field_floats);

    if (!snd_begin())
        return "couldn't record a run";

    ID3D12GraphicsCommandList *list = (ID3D12GraphicsCommandList *)snd.cmd;
    d3d12_tlas_build(snd.cmd, ID3D12Resource_GetGPUVirtualAddress(snd.instances), (uint32_t)n, snd_build_flags,
                     snd.tlas, ID3D12Resource_GetGPUVirtualAddress(snd.scratch));
    d3d12_as_barrier(snd.cmd, &snd.tlas_pool);

    const d3d12_set_t *sets[1] = { &snd.set };
    d3d12_compute_begin(list, D3D12_PL_SOUND);
    d3d12_compute_bind_sets(list, D3D12_PL_SOUND, sets);

    const uint64_t field_bytes = (uint64_t)t->num_sources * field_floats * sizeof(float);
    const uint64_t diff_bytes = (uint64_t)t->num_queries * SOUND_DIFFRACT_FLOATS * sizeof(float);

    if (t->num_sources > 0) {
        ID3D12GraphicsCommandList4_CopyBufferRegion(snd.cmd, snd.fields, 0, snd.zeros, 0, field_bytes);
        snd_transition(snd.fields, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList4_SetPipelineState(snd.cmd, snd.pipeline);
        d3d12_compute_dispatch(list, D3D12_PL_SOUND, groups, (uint32_t)t->num_sources, 1);
    }

    if (t->num_queries > 0) {
        ID3D12GraphicsCommandList4_SetPipelineState(snd.cmd, snd.diff_pipeline);
        d3d12_compute_dispatch(list, D3D12_PL_SOUND, (uint32_t)t->num_queries, 1, 1);
    }

    if (t->num_sources > 0) {
        snd_transition(snd.fields, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList4_CopyBufferRegion(snd.cmd, snd.readback, 0, snd.fields, 0, field_bytes);
    }
    if (t->num_queries > 0) {
        snd_transition(snd.diff, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList4_CopyBufferRegion(snd.cmd, snd.diff_readback, 0, snd.diff, 0, diff_bytes);
    }

    if (!snd_submit())
        return snd.why ? snd.why : "a run failed";

    vkpt_sound_unpack(t, field_floats, snd.readback_map, snd.diff_readback_map);
    return NULL;
}

bool d3d12_sound_trace(const r_sound_trace_t *trace, const char **why)
{
    const char *reason;

    AcquireSRWLockExclusive(&snd_lock);
    if (!snd.up)
        reason = snd.why ? snd.why : "the renderer is not up";
    else if (snd.why)
        reason = snd.why;
    else if (!snd.pipeline)
        reason = "no shader for it";
    else
        reason = trace ? snd_run(trace) : NULL;
    ReleaseSRWLockExclusive(&snd_lock);

    if (why)
        *why = reason;
    return !reason;
}

static void snd_destroy_objects(void)
{
    snd_free_scene();
    D3D12_RELEASE(snd.pipeline);
    D3D12_RELEASE(snd.diff_pipeline);
    D3D12_RELEASE(snd.params);
    D3D12_RELEASE(snd.fields);
    D3D12_RELEASE(snd.diff);
    D3D12_RELEASE(snd.zeros);
    D3D12_RELEASE(snd.readback);
    D3D12_RELEASE(snd.diff_readback);
    snd.params_map = snd.readback_map = snd.diff_readback_map = NULL;
    D3D12_RELEASE(snd.cmd);
    D3D12_RELEASE(snd.allocator);
    D3D12_RELEASE(snd.fence);
    D3D12_RELEASE(snd.queue);
    if (snd.event)
        CloseHandle(snd.event);
    snd.event = NULL;
}

static ID3D12PipelineState *snd_pipeline(const char *name, const wchar_t *debug_name)
{
    size_t size = 0;
    void *dxil = d3d12_load_shader(name, &size);
    if (!dxil)
        return NULL;
    ID3D12PipelineState *pso = d3d12_create_compute_pso(D3D12_PL_SOUND, dxil, size, debug_name);
    FS_FreeFile(dxil);
    if (!pso)
        Com_WPrintf("Sound tracing on the GPU: couldn't create the %s pipeline.\n", name);
    return pso;
}

static bool snd_create_objects(void)
{
    D3D12_COMMAND_QUEUE_DESC qd;
    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(ID3D12Device5_CreateCommandQueue(d3d.device, &qd, &IID_ID3D12CommandQueue, (void **)&snd.queue)))
        return false;
    d3d12_set_name((ID3D12Object *)snd.queue, "sound queue");
    if (FAILED(ID3D12Device5_CreateCommandAllocator(d3d.device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                    &IID_ID3D12CommandAllocator, (void **)&snd.allocator)) ||
        FAILED(ID3D12Device5_CreateCommandList(d3d.device, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE, snd.allocator, NULL,
                                               &IID_ID3D12GraphicsCommandList4, (void **)&snd.cmd)) ||
        FAILED(ID3D12GraphicsCommandList4_Close(snd.cmd)) ||
        FAILED(ID3D12Device5_CreateFence(d3d.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                         (void **)&snd.fence)))
        return false;
    snd.fence_value = 0;
    snd.event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!snd.event)
        return false;

    if (!d3d12_set_alloc(&snd.set, D3D12_SET_SOUND, "sound"))
        return false;

    // a CBV is a multiple of 256 bytes, and so is the buffer behind it here
    const uint64_t params_size = SND_ALIGN_UP(sizeof(vkpt_sound_params_t), 256);
    snd.params = snd_buffer(params_size, D3D12_HEAP_TYPE_UPLOAD, false, &snd.params_map);
    snd.fields = snd_buffer(VKPT_SOUND_FIELDS_SIZE, D3D12_HEAP_TYPE_DEFAULT, true, NULL);
    snd.zeros = snd_buffer(VKPT_SOUND_FIELDS_SIZE, D3D12_HEAP_TYPE_DEFAULT, false, NULL);  // committed: zeroed
    snd.readback = snd_buffer(VKPT_SOUND_FIELDS_SIZE, D3D12_HEAP_TYPE_READBACK, false, &snd.readback_map);
    snd.diff = snd_buffer(VKPT_SOUND_DIFF_SIZE, D3D12_HEAP_TYPE_DEFAULT, true, NULL);
    snd.diff_readback = snd_buffer(VKPT_SOUND_DIFF_SIZE, D3D12_HEAP_TYPE_READBACK, false, &snd.diff_readback_map);
    if (!snd.params || !snd.fields || !snd.zeros || !snd.readback || !snd.diff || !snd.diff_readback)
        return false;

    d3d12_set_write_cbv(&snd.set, 1, 0, ID3D12Resource_GetGPUVirtualAddress(snd.params), (uint32_t)params_size);
    d3d12_set_write_buffer(&snd.set, 3, 0, snd.fields, 0, VKPT_SOUND_FIELDS_SIZE);
    d3d12_set_write_buffer(&snd.set, 5, 0, snd.diff, 0, VKPT_SOUND_DIFF_SIZE);

    // without them the device is still set up, and each run says why (as vkpt)
    snd.pipeline = snd_pipeline("sound_trace.comp", L"sound_trace.comp");
    snd.diff_pipeline = snd_pipeline("sound_diffract.comp", L"sound_diffract.comp");
    return true;
}

void d3d12_sound_init(void)
{
    AcquireSRWLockExclusive(&snd_lock);
    memset(&snd, 0, sizeof(snd));

    if (d3d.caps.raytracing_tier < D3D12_RAYTRACING_TIER_1_1)
        snd.why = "it needs ray queries (DXR 1.1)";
    else if (VKPT_SOUND_FIELD_FLOATS * sizeof(uint32_t) > D3D12_CS_TGSM_REGISTER_COUNT * sizeof(uint32_t))
        snd.why = "this GPU has too little shared memory for it";
    else if (!snd_create_objects()) {
        snd_destroy_objects();
        snd.why = "couldn't set it up";
    }
    snd.up = !snd.why;
    if (snd.why)
        Com_DPrintf("Sound tracing on the GPU: not available, %s.\n", snd.why);

    ReleaseSRWLockExclusive(&snd_lock);

    // The sound menu offers "hardware: GPU" only when this is 1, and steamaudio.c
    // sets up for the CPU otherwise (SA_InitSimulation).
    Cvar_FullSet("s_gpu_trace_available", snd.up ? "1" : "0", CVAR_ROM, FROM_CODE);
}

void d3d12_sound_shutdown(void)
{
    AcquireSRWLockExclusive(&snd_lock);
    snd_wait_idle();
    snd_destroy_objects();
    snd.up = false;
    ReleaseSRWLockExclusive(&snd_lock);

    // the GL renderer, or none, may come next
    Cvar_FullSet("s_gpu_trace_available", "0", CVAR_ROM, FROM_CODE);
}
