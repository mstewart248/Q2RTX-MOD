/*
The 3D view's compute passes - see passes.h.
*/

#include "passes.h"

#include <math.h>
#include <string.h>

#include "../vkpt/shader/constants.h"   // GRAD_DWN
#include "../vkpt/shader/froxel_shared.h"   // FROXEL_GRID_*, FROXEL_*GROUP_*

const d3d12_pass_info_t d3d12_pass_info[D3D12_NUM_PASSES] = {
    [D3D12_PASS_PRIMARY_RAYS]               = { D3D12_PL_PATH_TRACER, "primary_rays.rgen.query" },
    // path_tracer.c's PIPELINE_REFLECT_REFRACT_1/2_NOFLOW (spec 0 = bounce index,
    // 1 = Flow): a frame with no Flow grid, the march compiled out (pt_reflect_noflow)
    [D3D12_PASS_REFLECT_REFRACT_FIRST]      = { D3D12_PL_PATH_TRACER, "reflect_refract.rgen.query.spec_1_0" },
    [D3D12_PASS_REFLECT_REFRACT_SECOND]     = { D3D12_PL_PATH_TRACER, "reflect_refract.rgen.query.spec_0_1_1_0" },
    [D3D12_PASS_DIRECT_LIGHTING]            = { D3D12_PL_PATH_TRACER, "direct_lighting.rgen.query" },
    [D3D12_PASS_DIRECT_LIGHTING_CAUSTICS]   = { D3D12_PL_PATH_TRACER, "direct_lighting.rgen.query.spec_0_1" },
    [D3D12_PASS_INDIRECT_LIGHTING_FIRST]    = { D3D12_PL_PATH_TRACER, "indirect_lighting.rgen.query" },
    [D3D12_PASS_INDIRECT_LIGHTING_SECOND]   = { D3D12_PL_PATH_TRACER, "indirect_lighting.rgen.query.spec_0_1" },
    [D3D12_PASS_RESTIR_GI]                  = { D3D12_PL_PATH_TRACER, "restir_gi.rgen.query" },
    [D3D12_PASS_GRADIENT_REPROJECT]         = { D3D12_PL_STANDARD, "asvgf_gradient_reproject.comp" },
    [D3D12_PASS_GRADIENT_IMG]               = { D3D12_PL_STANDARD, "asvgf_gradient_img.comp" },
    [D3D12_PASS_GRADIENT_ATROUS]            = { D3D12_PL_STANDARD, "asvgf_gradient_atrous.comp" },
    [D3D12_PASS_TEMPORAL]                   = { D3D12_PL_STANDARD, "asvgf_temporal.comp" },
    [D3D12_PASS_ATROUS_LF]                  = { D3D12_PL_STANDARD, "asvgf_lf.comp" },
    [D3D12_PASS_ATROUS_0]                   = { D3D12_PL_STANDARD, "asvgf_atrous.comp" },
    [D3D12_PASS_ATROUS_1]                   = { D3D12_PL_STANDARD, "asvgf_atrous.comp.spec_0_1" },
    [D3D12_PASS_ATROUS_2]                   = { D3D12_PL_STANDARD, "asvgf_atrous.comp.spec_0_2" },
    [D3D12_PASS_ATROUS_3]                   = { D3D12_PL_STANDARD, "asvgf_atrous.comp.spec_0_3" },
    [D3D12_PASS_COMPOSITING]                = { D3D12_PL_STANDARD, "compositing.comp" },
    [D3D12_PASS_INTERLEAVE]                 = { D3D12_PL_STANDARD, "checkerboard_interleave.comp" },
    [D3D12_PASS_TAAU]                       = { D3D12_PL_STANDARD, "asvgf_taau.comp" },
    [D3D12_PASS_TM_HISTOGRAM]               = { D3D12_PL_STANDARD, "tone_mapping_histogram.comp" },
    [D3D12_PASS_TM_CURVE]                   = { D3D12_PL_STANDARD, "tone_mapping_curve.comp" },
    [D3D12_PASS_TM_APPLY]                   = { D3D12_PL_STANDARD, "tone_mapping_apply.comp" },  // SDR: spec 0 = 0
    [D3D12_PASS_TM_APPLY_HDR]               = { D3D12_PL_STANDARD, "tone_mapping_apply.comp.spec_0_1" },
    // vertex_buffer.c's pipelines, on its ubo + vertex buffer layout
    [D3D12_PASS_INSTANCE_GEOMETRY]          = { D3D12_PL_UBO_VERTEX_BUFFER, "instance_geometry.comp" },
    [D3D12_PASS_ANIMATE_MATERIALS]          = { D3D12_PL_UBO_VERTEX_BUFFER, "animate_materials.comp" },
    // path_tracer.c's PIPELINE_SHARC_UPDATE, and vertex_buffer.c's resolve
    [D3D12_PASS_SHARC_UPDATE]               = { D3D12_PL_PATH_TRACER, "sharc_update.rgen.query" },
    [D3D12_PASS_SHARC_RESOLVE]              = { D3D12_PL_UBO_VERTEX_BUFFER, "sharc_resolve.comp" },
    [D3D12_PASS_BLOOM_DOWNSCALE]            = { D3D12_PL_STANDARD, "bloom_downscale.comp" },
    [D3D12_PASS_BLOOM_BLUR]                 = { D3D12_PL_STANDARD, "bloom_blur.comp" },
    [D3D12_PASS_BLOOM_COMPOSITE]            = { D3D12_PL_STANDARD, "bloom_composite.comp" },
    [D3D12_PASS_DLSSG_HDR]                  = { D3D12_PL_STANDARD, "dlssg_hdr.comp" },
    // god_rays.c's GOD_RAYS_PIPELINE_*
    [D3D12_PASS_GOD_RAYS]                   = { D3D12_PL_GOD_RAYS, "god_rays.comp" },
    [D3D12_PASS_GOD_RAYS_FILTER]            = { D3D12_PL_GOD_RAYS, "god_rays_filter.comp" },
    [D3D12_PASS_FROXEL_RESERVOIR]           = { D3D12_PL_GOD_RAYS, "froxel_reservoir.comp" },
    [D3D12_PASS_FROXEL_SPATIAL]             = { D3D12_PL_GOD_RAYS, "froxel_spatial.comp" },
    [D3D12_PASS_SKY_EXPOSURE_BAKE]          = { D3D12_PL_GOD_RAYS, "sky_exposure_bake.comp" },
    [D3D12_PASS_FROXEL_SCATTER]             = { D3D12_PL_GOD_RAYS, "froxel_scatter.comp" },
    [D3D12_PASS_FROXEL_INTEGRATE]           = { D3D12_PL_GOD_RAYS, "froxel_integrate.comp" },
    // motion_blur.c's and chromatic_aberration.c's, and the copy back vkpt makes
    // with vkCmdCopyImage (the D3D12 renderer's own shader, d3d12/shader)
    [D3D12_PASS_MOTION_BLUR]                = { D3D12_PL_STANDARD, "motion_blur.comp" },
    [D3D12_PASS_CHROMATIC_ABERRATION]       = { D3D12_PL_STANDARD, "chromatic_aberration.comp" },
    [D3D12_PASS_POSTFX_COPY]                = { D3D12_PL_STANDARD, "postfx_copy.comp" },
    [D3D12_PASS_DLSS5_CONVERT]              = { D3D12_PL_STANDARD, "dlss5_convert.comp" },
    // draw.c's crt_downsample, on its ubo + textures layout (world.c's blit records it)
    [D3D12_PASS_CRT_DOWNSAMPLE]             = { D3D12_PL_UBO_TEXTURES, "crt_downsample.comp" },
    // NVIDIA Flow: path_tracer.c's PIPELINE_PRIMARY_RAYS_FLOW_SPLIT and PIPELINE_FLOW_PASS
    // (spec 1 = spec_flow_mode: 1 the primary rays without the march, 2 the march alone -
    // frozen, see src/CMakeLists.txt), PIPELINE_REFLECT_REFRACT_1/2 with the march, and
    // flow.c's pipelines on their layouts
    [D3D12_PASS_PRIMARY_RAYS_FLOW_SPLIT]    = { D3D12_PL_PATH_TRACER, "primary_rays.rgen.query.spec_1_1" },
    [D3D12_PASS_FLOW_MARCH]                 = { D3D12_PL_PATH_TRACER, "primary_rays.rgen.query.spec_1_2" },
    [D3D12_PASS_REFLECT_REFRACT_FIRST_FLOW] = { D3D12_PL_PATH_TRACER, "reflect_refract.rgen.query" },
    [D3D12_PASS_REFLECT_REFRACT_SECOND_FLOW] = { D3D12_PL_PATH_TRACER, "reflect_refract.rgen.query.spec_0_1" },
    [D3D12_PASS_FLOW_OCCUPANCY]             = { D3D12_PL_UBO_TEXTURES, "flow_occupancy.comp" },
    [D3D12_PASS_FLOW_LIGHTS]                = { D3D12_PL_UBO_TEXTURES, "flow_lights.comp" },
    [D3D12_PASS_FLOW_AMBIENT]               = { D3D12_PL_FLOW_ENV, "flow_ambient.comp" },
};

void d3d12_barrier_all(ID3D12GraphicsCommandList4 *cmd, bool enhanced)
{
    ID3D12GraphicsCommandList7 *cmd7 = NULL;
    if (enhanced &&
        SUCCEEDED(ID3D12GraphicsCommandList4_QueryInterface(cmd, &IID_ID3D12GraphicsCommandList7, (void **)&cmd7))) {
        D3D12_GLOBAL_BARRIER gb = {
            D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
        };
        D3D12_BARRIER_GROUP g = { D3D12_BARRIER_TYPE_GLOBAL, 1 };
        g.pGlobalBarriers = &gb;
        ID3D12GraphicsCommandList7_Barrier(cmd7, 1, &g);
        ID3D12GraphicsCommandList7_Release(cmd7);
    } else {
        D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
        ID3D12GraphicsCommandList4_ResourceBarrier(cmd, 1, &b);
    }
}

// zeros over [offset, offset + size) of a buffer, by copy
static void zero_range(ID3D12GraphicsCommandList4 *cmd, ID3D12Resource *dst, uint64_t offset, uint64_t size,
                       ID3D12Resource *zeros)
{
    for (uint64_t at = 0; at < size; at += D3D12_ZERO_BUFFER_SIZE) {
        const uint64_t n = size - at < D3D12_ZERO_BUFFER_SIZE ? size - at : D3D12_ZERO_BUFFER_SIZE;
        ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, dst, offset + at, zeros, 0, n);
    }
}

static bool blood_region_ok(const d3d12_blood_upload_t *u, const d3d12_blood_region_t *g)
{
    return u->staging_prims && u->staging_positions && g->count && g->dst >= u->first && g->dst <= u->end &&
           g->count <= u->end - g->dst && g->src <= u->staging_capacity && g->count <= u->staging_capacity - g->src;
}

bool d3d12_record_blood_upload(ID3D12GraphicsCommandList4 *cmd, const d3d12_blood_upload_t *u)
{
    if (!u->prims || !u->positions || u->first >= u->end)
        return false;
    bool wrote = false;
    if (u->zero_section && u->zeros) {
        uint32_t at = u->first;
        for (uint32_t r = 0; r <= u->num_regions; r++) {
            if (r < u->num_regions && !blood_region_ok(u, &u->regions[r]))
                continue;
            const uint32_t next = r < u->num_regions ? u->regions[r].dst : u->end;
            if (next > at) {
                zero_range(cmd, u->prims, (uint64_t)at * u->prim_size, (uint64_t)(next - at) * u->prim_size, u->zeros);
                zero_range(cmd, u->positions, (uint64_t)at * u->position_size,
                           (uint64_t)(next - at) * u->position_size, u->zeros);
                wrote = true;
            }
            if (r < u->num_regions && u->regions[r].dst + u->regions[r].count > at)
                at = u->regions[r].dst + u->regions[r].count;
        }
    }
    for (uint32_t r = 0; r < u->num_regions; r++) {
        const d3d12_blood_region_t *g = &u->regions[r];
        if (!blood_region_ok(u, g))
            continue;
        ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, u->prims, (uint64_t)g->dst * u->prim_size, u->staging_prims,
                                                    (uint64_t)g->src * u->prim_size, (uint64_t)g->count * u->prim_size);
        ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, u->positions, (uint64_t)g->dst * u->position_size,
                                                    u->staging_positions, (uint64_t)g->src * u->position_size,
                                                    (uint64_t)g->count * u->position_size);
        wrote = true;
    }
    return wrote;
}

void d3d12_clear_buffer(ID3D12GraphicsCommandList4 *cmd, ID3D12Resource *buffer, uint64_t size,
                        ID3D12Resource *zeros)
{
    for (uint64_t at = 0; at < size; at += D3D12_ZERO_BUFFER_SIZE) {
        const uint64_t n = size - at < D3D12_ZERO_BUFFER_SIZE ? size - at : D3D12_ZERO_BUFFER_SIZE;
        ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, buffer, at, zeros, 0, n);
    }
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = buffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList4_ResourceBarrier(cmd, 1, &b);
}

/* ------------------------------------------------------------------------- */

typedef struct {
    const d3d12_passes_t    *p;
    int                     layout;     // the root signature set, -1 = none yet
    bool                    tile_args_read;     // tile_args is in INDIRECT_ARGUMENT
} recorder_t;

static void use_layout(recorder_t *r, d3d12_pipeline_layout_id_t layout)
{
    if (r->layout == (int)layout)
        return;     // root arguments survive a PSO change, not a root signature change
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)r->p->cmd;
    d3d12_compute_begin(gl, layout);
    const d3d12_set_t *ubo_vertex_buffer[2] = { r->p->standard_sets[0], r->p->standard_sets[2] };
    // (the ubo + textures layout takes the standard sets' first two)
    d3d12_compute_bind_sets(gl, layout, layout == D3D12_PL_PATH_TRACER       ? r->p->pt_sets :
                                        layout == D3D12_PL_UBO_VERTEX_BUFFER ? ubo_vertex_buffer :
                                        layout == D3D12_PL_GOD_RAYS          ? r->p->god_rays_sets :
                                        layout == D3D12_PL_FLOW_ENV          ? r->p->flow_env_sets :
                                                                               r->p->standard_sets);
    r->layout = (int)layout;
}

// shader/path_tracer.h's push_constant_block
typedef struct {
    int32_t  gpu_index;
    int32_t  bounce_index;
    uint32_t tile_read_list;
    uint32_t tile_append_list;
    uint32_t tile_buffer[2];        // a buffer address on Vulkan; set 15 here
} pt_push_t;

// path_tracer.c's dispatch_rays on one GPU (ray query): 8x8 groups over
// width x height, `depth` deep, adding tiles to list append_list - 1 (0: none)
static void trace_grid_append(recorder_t *r, d3d12_pass_t pass, int bounce, uint32_t width, uint32_t height,
                              uint32_t depth, uint32_t append_list)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)r->p->cmd;
    ID3D12GraphicsCommandList4_SetPipelineState(r->p->cmd, r->p->psos[pass]);
    use_layout(r, D3D12_PL_PATH_TRACER);
    pt_push_t push = { 0 };
    push.gpu_index = -1;
    push.bounce_index = bounce;
    push.tile_append_list = append_list;
    d3d12_compute_push_constants(gl, D3D12_PL_PATH_TRACER, &push, sizeof(push));
    d3d12_compute_dispatch(gl, D3D12_PL_PATH_TRACER, (width + 7) / 8, (height + 7) / 8, depth);
    d3d12_barrier_all(r->p->cmd, r->p->enhanced_barriers);
}

static void trace_grid(recorder_t *r, d3d12_pass_t pass, int bounce, uint32_t width, uint32_t height,
                       uint32_t depth)
{
    trace_grid_append(r, pass, bounce, width, height, depth, 0);
}

// ...as the lighting passes dispatch it: both checkerboard fields side by side (z = field)
static void trace(recorder_t *r, d3d12_pass_t pass, int bounce, uint32_t width, uint32_t height)
{
    trace_grid(r, pass, bounce, width, height, 2);
}

/* trace() as a ray generation shader (SER, pt_ser; spv2dxil's to_raygen): the same root
   arguments and push constants, and DispatchRays over every thread the compute groups
   had - whole 8x8 groups - so each invocation gets the launch ID it has there. The
   global root signature is the path tracer layout's, so nothing is bound again. */
static void trace_raygen(recorder_t *r, ID3D12StateObject *so, D3D12_GPU_VIRTUAL_ADDRESS record, int bounce,
                         uint32_t width, uint32_t height)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)r->p->cmd;
    use_layout(r, D3D12_PL_PATH_TRACER);
    ID3D12GraphicsCommandList4_SetPipelineState1(r->p->cmd, so);
    pt_push_t push = { 0 };
    push.gpu_index = -1;
    push.bounce_index = bounce;
    d3d12_compute_push_constants(gl, D3D12_PL_PATH_TRACER, &push, sizeof(push));

    D3D12_DISPATCH_RAYS_DESC rays;
    memset(&rays, 0, sizeof(rays));
    rays.RayGenerationShaderRecord.StartAddress = record;
    rays.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    rays.Width = (width + 7) / 8 * 8;
    rays.Height = (height + 7) / 8 * 8;
    rays.Depth = 2;
    ID3D12GraphicsCommandList4_DispatchRays(r->p->cmd, &rays);
    d3d12_barrier_all(r->p->cmd, r->p->enhanced_barriers);
}

// the profiler's rows by name, for the markers (vkpt/profiler.h, then passes.h's pieces)
static const char *const timer_names[D3D12_NUM_PROFILER_ENTRIES] = {
#define PROFILER_DO(a, ...) #a,
    PROFILER_LIST
#undef PROFILER_DO
    "SHADOW_MAP_DYNAMIC", "BVH_TLAS", "FLOW_PASSES",
};

/* d3d12_dred 2 (device.c): a marker per mark, which DRED's breadcrumbs keep with the
   operation it came before, so a report of where the GPU stopped names the pass. A
   PIX-style marker: metadata 0 is a UTF-16 string. */
static void timer_marker(ID3D12GraphicsCommandList4 *cmd, int entry, int action)
{
    const char *name = entry >= 0 && entry < D3D12_NUM_PROFILER_ENTRIES && timer_names[entry] ?
                       timer_names[entry] : "?";
    if (!strncmp(name, "PROFILER_", 9))
        name += 9;
    const char *what = action == PROFILER_START ? " start" : " end";
    WCHAR text[96];
    UINT n = 0;
    for (const char *s = name; *s && n < 80; s++)
        text[n++] = (WCHAR)*s;
    for (const char *s = what; *s && n < 95; s++)
        text[n++] = (WCHAR)*s;
    text[n++] = 0;
    ID3D12GraphicsCommandList4_SetMarker(cmd, 0, text, n * (UINT)sizeof(WCHAR));
}

void d3d12_timer_mark(ID3D12GraphicsCommandList4 *cmd, const d3d12_timers_t *t, int entry, int action)
{
    if (t && t->markers)
        timer_marker(cmd, entry, action);
    if (!t || !t->heap)
        return;
    const uint32_t q = (uint32_t)(entry * 2 + action);
    ID3D12GraphicsCommandList4_EndQuery(cmd, t->heap, D3D12_QUERY_TYPE_TIMESTAMP, t->base + q);
    t->used[q] = true;
}

// vkpt's BEGIN_PERF_MARKER (PROFILER_START) and END_PERF_MARKER (PROFILER_STOP)
static void mark(recorder_t *r, int entry, int action)
{
    d3d12_timer_mark(r->p->cmd, &r->p->timers, entry, action);
}

// a compute pass of vkpt's general layout (asvgf.c, tone_mapping.c): x * y groups
static void pass(recorder_t *r, d3d12_pass_t pass, const void *push, size_t push_size, uint32_t x, uint32_t y)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)r->p->cmd;
    ID3D12GraphicsCommandList4_SetPipelineState(r->p->cmd, r->p->psos[pass]);
    use_layout(r, D3D12_PL_STANDARD);
    if (push_size)
        d3d12_compute_push_constants(gl, D3D12_PL_STANDARD, push, push_size);
    d3d12_compute_dispatch(gl, D3D12_PL_STANDARD, x, y, 1);
    d3d12_barrier_all(r->p->cmd, r->p->enhanced_barriers);
}

// a pass of vertex_buffer.c's layout (ubo, vertex buffer): x groups
static void geometry_pass(recorder_t *r, d3d12_pass_t pass, uint32_t x)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)r->p->cmd;
    ID3D12GraphicsCommandList4_SetPipelineState(r->p->cmd, r->p->psos[pass]);
    use_layout(r, D3D12_PL_UBO_VERTEX_BUFFER);
    d3d12_compute_dispatch(gl, D3D12_PL_UBO_VERTEX_BUFFER, x, 1, 1);
    d3d12_barrier_all(r->p->cmd, r->p->enhanced_barriers);
}

void d3d12_record_instance_geometry(const d3d12_passes_t *p, uint32_t num_instances, uint32_t num_world_prims)
{
    recorder_t r = { p, -1 };
    if (num_instances)
        geometry_pass(&r, D3D12_PASS_INSTANCE_GEOMETRY, num_instances);
    if (num_world_prims)
        geometry_pass(&r, D3D12_PASS_ANIMATE_MATERIALS, (num_world_prims + 255) / 256);
}

void d3d12_record_dlssg_convert(const d3d12_passes_t *p, uint32_t mode, uint32_t target, uint32_t width,
                                uint32_t height)
{
    // dlssg_hdr.c's dlssg_hdr_push_constants, and its 16 x 16 groups
    const uint32_t push[4] = { mode, target, width, height };
    recorder_t r = { p, -1 };
    pass(&r, D3D12_PASS_DLSSG_HDR, push, sizeof(push), (width + 15) / 16, (height + 15) / 16);
}

// vkpt_pt_sharc_update: the cache zeroed when it starts afresh (vkpt_sharc_clear),
// one training path per stride x stride block of the whole render (not per
// field), then sharc_resolve over every entry. The barriers after each are
// vkpt's sharc_barrier.
static void sharc_update(recorder_t *r, const d3d12_passes_frame_t *f)
{
    const d3d12_passes_t *p = r->p;
    mark(r, PROFILER_SHARC, PROFILER_START);
    if (f->sharc_clear)
        for (int i = 0; i < 3; i++)
            d3d12_clear_buffer(p->cmd, p->sharc_buffers[i], p->sharc_sizes[i], p->zero_buffer);
    const uint32_t s = f->sharc_stride < 1 ? 1 : f->sharc_stride > 16 ? 16 : f->sharc_stride;
    trace_grid(r, D3D12_PASS_SHARC_UPDATE, 0, (f->render_width + s - 1) / s, (f->render_height + s - 1) / s, 1);
    geometry_pass(r, D3D12_PASS_SHARC_RESOLVE, p->sharc_entries / 256);
    mark(r, PROFILER_SHARC, PROFILER_STOP);
}

static void trace_paths(recorder_t *r, d3d12_pass_t pass, int bounce, uint32_t list, uint32_t append);

// vkpt_pt_trace_lighting (`tiles`: the view has the tile lists, so maybe path lists)
static void trace_lighting(recorder_t *r, const d3d12_passes_frame_t *f, bool tiles)
{
    /* SPARSE RENDERING (pt_sparse, shader/constants.h; path_tracer.c's): direct lighting
       covers the grid, where the pixels this frame skips get their guides and the rest
       fill the sparse path list, then runs again over that list, and so does the first
       bounce. Without the list the shaders still skip the same pixels, one dispatch over
       the grid each. */
    const bool sparse = tiles && f->sparse;
    const d3d12_pass_t direct = f->caustics ? D3D12_PASS_DIRECT_LIGHTING_CAUSTICS : D3D12_PASS_DIRECT_LIGHTING;

    mark(r, PROFILER_DIRECT_LIGHTING, PROFILER_START);
    if (sparse) {
        trace_grid_append(r, direct, 0, f->field_width, f->render_height, 2, PT_PATH_LIST_SPARSE);
        trace_paths(r, direct, 0, PT_PATH_LIST_SPARSE, 0);
    } else {
        trace(r, direct, 0, f->field_width, f->render_height);
    }
    mark(r, PROFILER_DIRECT_LIGHTING, PROFILER_STOP);

    mark(r, PROFILER_INDIRECT_LIGHTING, PROFILER_START);
    if (f->num_bounce_rays > 0) {
        const uint32_t height = f->num_bounce_rays == 0.5f ? f->render_height / 2 : f->render_height;
        const int num_bounces = (int)ceilf(f->num_bounce_rays);
        // pt_bounce_cull's path lists (trace_paths); not under SER, whose raygen
        // dispatch covers the grid. pt_sparse's first bounce walks the sparse list (not
        // at half resolution, whose rows it renames), and then the later bounces have to
        // take the lists too, as it never visits the other pixels to end their paths.
        const bool no_ser = !r->p->raygen[0] && !r->p->raygen[1];
        const bool sparse_first = sparse && no_ser && f->num_bounce_rays >= 1.f;
        const bool paths = tiles && num_bounces >= 2 && num_bounces <= PT_PATH_LISTS && no_ser &&
                           (f->paths || sparse_first);
        for (int bounce = 0; bounce < num_bounces; bounce++) {
            // INDIRECT_LIGHTING_0 is the first bounce, _1 every later one (path_tracer.c's)
            if (bounce <= 1)
                mark(r, PROFILER_INDIRECT_LIGHTING_0 + bounce, PROFILER_START);
            const int k = bounce ? 1 : 0;
            const d3d12_pass_t pass = bounce ? D3D12_PASS_INDIRECT_LIGHTING_SECOND : D3D12_PASS_INDIRECT_LIGHTING_FIRST;
            const uint32_t append = bounce + 1 < num_bounces ? (uint32_t)bounce + 1 : 0;
            if (sparse_first && bounce == 0)
                trace_paths(r, pass, 0, PT_PATH_LIST_SPARSE, paths ? append : 0);
            else if (paths && bounce > 0)
                trace_paths(r, pass, bounce, (uint32_t)bounce, append);
            else if (paths)
                trace_grid_append(r, pass, bounce, f->field_width, height, 2, append);
            else if (r->p->raygen[k])
                trace_raygen(r, r->p->raygen[k], r->p->raygen_records[k], bounce, f->field_width, height);
            else
                trace(r, pass, bounce, f->field_width, height);
            if (bounce == 0 || bounce == num_bounces - 1)
                mark(r, PROFILER_INDIRECT_LIGHTING_0 + (bounce ? 1 : 0), PROFILER_STOP);
        }
    }
    mark(r, PROFILER_INDIRECT_LIGHTING, PROFILER_STOP);

    // its temporal pass, then the spatial one
    if (f->restir_gi && f->num_bounce_rays >= 1) {
        mark(r, PROFILER_RESTIR_GI, PROFILER_START);
        trace(r, D3D12_PASS_RESTIR_GI, 0, f->field_width, f->render_height);
        if (f->restir_gi_spatial)
            trace(r, D3D12_PASS_RESTIR_GI, 1, f->field_width, f->render_height);
        mark(r, PROFILER_RESTIR_GI, PROFILER_STOP);
    }
}

// vkpt_asvgf_filter
static void asvgf_filter(recorder_t *r, const d3d12_passes_frame_t *f, bool enable_lf)
{
    const uint32_t gx = (f->slice_width / GRAD_DWN + 15) / 16;
    const uint32_t gy = (f->render_height / GRAD_DWN + 15) / 16;
    const uint32_t fx = (f->slice_width + 15) / 16;
    const uint32_t fy = (f->render_height + 15) / 16;

    mark(r, PROFILER_ASVGF_RECONSTRUCT_GRADIENT, PROFILER_START);
    pass(r, D3D12_PASS_GRADIENT_IMG, NULL, 0, gx, gy);
    for (uint32_t i = 0; i < 7; i++)
        pass(r, D3D12_PASS_GRADIENT_ATROUS, &i, sizeof(i), gx, gy);
    mark(r, PROFILER_ASVGF_RECONSTRUCT_GRADIENT, PROFILER_STOP);

    mark(r, PROFILER_ASVGF_TEMPORAL, PROFILER_START);
    pass(r, D3D12_PASS_TEMPORAL, NULL, 0, (f->slice_width + 14) / 15, (f->render_height + 14) / 15);
    mark(r, PROFILER_ASVGF_TEMPORAL, PROFILER_STOP);

    mark(r, PROFILER_ASVGF_ATROUS, PROFILER_START);
    for (uint32_t i = 0; i < 4; i++) {
        if (enable_lf)
            pass(r, D3D12_PASS_ATROUS_LF, &i, sizeof(i), gx, gy);
        pass(r, (d3d12_pass_t)(D3D12_PASS_ATROUS_0 + i), NULL, 0, fx, fy);
    }
    mark(r, PROFILER_ASVGF_ATROUS, PROFILER_STOP);
}

// MOTION_BLUR's first width x height back over TAA_OUTPUT, or DLSS_OUTPUT: motion
// blur's and chromatic aberration's vkCmdCopyImage, as a pass (postfx_copy.comp)
static void postfx_copy(recorder_t *r, bool dlss, uint32_t width, uint32_t height)
{
    const int32_t push[3] = { (int32_t)width, (int32_t)height, dlss ? 1 : 0 };
    pass(r, D3D12_PASS_POSTFX_COPY, push, sizeof(push), (width + 15) / 16, (height + 15) / 16);
}

// vkpt_bloom_record_cmd_buffer, at width x height (the
// TAA output, or with DLSS the output): TAA_OUTPUT (DLSS_OUTPUT) downscaled to a
// quarter, blurred across into BLOOM_HBLUR and back down into BLOOM_VBLUR (the
// DLSS_BLOOM_* pair), then blended back by the UBO's bloom_intensity - the
// shaders pick the images by the UBO's pt_dlss. `take_motion_blur`: the downscale
// also writes MOTION_BLUR back over DLSS_OUTPUT (motion blur's hand-off).
static void bloom(recorder_t *r, const d3d12_passes_frame_t *f, uint32_t width, uint32_t height,
                  bool take_motion_blur)
{
    // bloom.c's bloom_blur_push_constants
    typedef struct {
        float   pixstep_x, pixstep_y, argument_scale, normalization_scale;
        int32_t num_samples, pass;
    } blur_push_t;
    const uint32_t qx = (width / 4 + 15) / 16, qy = (height / 4 + 15) / 16;

    // bloom_debug: the stage asked for over the frame, and no composite
    const int32_t debug_push[4] = { (int32_t)width, (int32_t)height, f->dlss ? 4 : 3, 0 };
    const uint32_t gx = (width + 15) / 16, gy = (height + 15) / 16;

    const int32_t take = take_motion_blur ? 1 : 0;
    pass(r, D3D12_PASS_BLOOM_DOWNSCALE, &take, sizeof(take), qx, qy);
    if (f->bloom_debug == 1) {
        pass(r, D3D12_PASS_POSTFX_COPY, debug_push, sizeof(debug_push), gx, gy);
        return;
    }
    const blur_push_t across = { 1.f, 0.f, f->bloom_argument_scale, f->bloom_normalization_scale,
                                 f->bloom_num_samples, 0 };
    pass(r, D3D12_PASS_BLOOM_BLUR, &across, sizeof(across), qx, qy);
    if (f->bloom_debug == 2) {
        const int32_t hblur_push[4] = { debug_push[0], debug_push[1], debug_push[2], 1 };
        pass(r, D3D12_PASS_POSTFX_COPY, hblur_push, sizeof(hblur_push), gx, gy);
        return;
    }
    const blur_push_t down = { 0.f, 1.f, f->bloom_argument_scale, f->bloom_normalization_scale,
                               f->bloom_num_samples, 1 };
    pass(r, D3D12_PASS_BLOOM_BLUR, &down, sizeof(down), qx, qy);
    if (f->bloom_debug == 3) {
        pass(r, D3D12_PASS_POSTFX_COPY, debug_push, sizeof(debug_push), gx, gy);
        return;
    }
    pass(r, D3D12_PASS_BLOOM_COMPOSITE, NULL, 0, gx, gy);
}

// after an NGX call: it left its own heaps, root signature and pipeline on the list
// (ngx.h), so ours again, and what it wrote made visible
static void after_ngx(recorder_t *r)
{
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(r->p->cmd, r->p->num_heaps, r->p->heaps);
    r->layout = -1;     // NGX's root signature: ours afresh at the next pass
    d3d12_barrier_all(r->p->cmd, r->p->enhanced_barriers);
}

// vkpt_tone_mapping_record_cmd_buffer, SDR, on TAA_OUTPUT in place
static void tone_mapping(recorder_t *r, const d3d12_passes_frame_t *f, uint32_t width, uint32_t height)
{
    if (f->tm_reset)
        d3d12_clear_buffer(r->p->cmd, r->p->tonemap_buffer, r->p->tonemap_size, r->p->zero_buffer);

    const uint32_t x = (width + 15) / 16, y = (height + 15) / 16;
    pass(r, D3D12_PASS_TM_HISTOGRAM, NULL, 0, x, y);

    // the tone curve: the reset flag, the frame time, half of a symmetric
    // Gaussian over the curve's slopes (tone_mapping.c says why)
    float curve[16] = { f->tm_reset ? 1.f : 0.f, f->frame_time };
    const float sigma = f->tm_slope_blur_sigma;
    float sum = 0.f;
    for (int i = 0; i < 14; i++) {
        const float k = expf(-(float)(i * i) / (2.f * sigma * sigma));
        sum += k * (i == 0 ? 1.f : 2.f);
        curve[i + 2] = k;
    }
    for (int i = 0; i < 14; i++)
        curve[i + 2] /= sum;
    pass(r, D3D12_PASS_TM_CURVE, curve, sizeof(curve), 1, 1);

    // Reinhard blended in above tm_knee_start, white at tm_white_point
    const float knee_start = f->tm_knee_start, white = f->tm_white_point;
    const float knee_w = (knee_start * (knee_start - 2.f) + white) / (white - 1.f);
    const float apply[3] = { knee_w, -knee_start * knee_start, knee_w - 2.f * knee_start };
    pass(r, f->hdr ? D3D12_PASS_TM_APPLY_HDR : D3D12_PASS_TM_APPLY, apply, sizeof(apply), x, y);
}

/* ------------------------------------------------------------------------- */
/* THE FOG (god_rays.c)                                                       */
/* ------------------------------------------------------------------------- */

// a pass of god_rays.c's layout, x * y * z groups; its push constant is one int -
// the march's pass, the bake's first voxel, else 0
static void god_rays_pass(recorder_t *r, d3d12_pass_t pass, uint32_t push, uint32_t x, uint32_t y, uint32_t z)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)r->p->cmd;
    ID3D12GraphicsCommandList4_SetPipelineState(r->p->cmd, r->p->psos[pass]);
    use_layout(r, D3D12_PL_GOD_RAYS);
    d3d12_compute_push_constants(gl, D3D12_PL_GOD_RAYS, &push, sizeof(push));
    d3d12_compute_dispatch(gl, D3D12_PL_GOD_RAYS, x, y, z);
    d3d12_barrier_all(r->p->cmd, r->p->enhanced_barriers);
}

// vkpt_record_god_rays_trace_command_buffer: 16x16 groups (god_rays.c's
// THREAD_GROUP_SIZE) of a thread per 2x2 block, or in photo mode a thread per pixel
static void god_rays_march(recorder_t *r, const d3d12_passes_frame_t *f, uint32_t pass)
{
    const uint32_t g = f->fog_accum_march ? 16 : 32;
    god_rays_pass(r, D3D12_PASS_GOD_RAYS, pass, (f->slice_width + g - 1) / g, (f->render_height + g - 1) / g, 1);
}

// vkpt_record_froxel_command_buffer: the light selection (pt_fog_restir), then the
// shading - a batch of the sky exposure bake, the scatter and the integration. The
// cross-frame barrier vkpt puts at its head (pt_fog_xframe_barrier) is the barrier
// after the pass before it here: command lists on one queue do not overlap.
static void froxel(recorder_t *r, const d3d12_passes_frame_t *f)
{
    const uint32_t gx = (FROXEL_GRID_X + FROXEL_GROUP_X - 1) / FROXEL_GROUP_X;
    const uint32_t gy = (FROXEL_GRID_Y + FROXEL_GROUP_Y - 1) / FROXEL_GROUP_Y;
    if (f->froxel_restir) {
        mark(r, PROFILER_FOG_FROXEL_RESERVOIR, PROFILER_START);
        god_rays_pass(r, D3D12_PASS_FROXEL_RESERVOIR, 0, gx, gy, FROXEL_GRID_Z);
        mark(r, PROFILER_FOG_FROXEL_RESERVOIR, PROFILER_STOP);
        mark(r, PROFILER_FOG_FROXEL_SPATIAL, PROFILER_START);
        god_rays_pass(r, D3D12_PASS_FROXEL_SPATIAL, 0, gx, gy, FROXEL_GRID_Z);
        mark(r, PROFILER_FOG_FROXEL_SPATIAL, PROFILER_STOP);
    }
    if (f->sky_exposure_count)
        god_rays_pass(r, D3D12_PASS_SKY_EXPOSURE_BAKE, f->sky_exposure_first,
                      (f->sky_exposure_count + SKY_EXPOSURE_BAKE_GROUP - 1) / SKY_EXPOSURE_BAKE_GROUP, 1, 1);
    mark(r, PROFILER_FOG_FROXEL_SCATTER, PROFILER_START);
    god_rays_pass(r, D3D12_PASS_FROXEL_SCATTER, 0, gx, gy, FROXEL_GRID_Z);
    mark(r, PROFILER_FOG_FROXEL_SCATTER, PROFILER_STOP);
    mark(r, PROFILER_FOG_FROXEL_INTEGRATE, PROFILER_START);
    god_rays_pass(r, D3D12_PASS_FROXEL_INTEGRATE, 0, (FROXEL_GRID_X + FROXEL_INT_GROUP_X - 1) / FROXEL_INT_GROUP_X,
                  (FROXEL_GRID_Y + FROXEL_INT_GROUP_Y - 1) / FROXEL_INT_GROUP_Y, 1);
    mark(r, PROFILER_FOG_FROXEL_INTEGRATE, PROFILER_STOP);
}

// vkpt_record_god_rays_filter_command_buffer: 16x16 groups over both fields
static void god_rays_filter(recorder_t *r, const d3d12_passes_frame_t *f)
{
    god_rays_pass(r, D3D12_PASS_GOD_RAYS_FILTER, 0, (f->slice_width + 15) / 16, (f->render_height + 15) / 16, 1);
}

/* TILE COMPACTION (path_tracer.c, shader/constants.h): the primary rays add each 8x8
   tile holding a reflect/refract surface to list 0, and bounce b walks list b with an
   indirect dispatch, adding the tiles whose next vertex is one to list b + 1. A tile
   runs the same 64 threads as on the full grid, so the picture is the same. Two
   things differ from vkpt's:
   - A bounce writes the lists as a UAV while its own arguments sit in them, and no
     D3D12 state is both UNORDERED_ACCESS and INDIRECT_ARGUMENT: each bounce's
     arguments are copied out to tile_args first.
   - A dispatch holds at most 65535 groups a dimension, and a 4K view at full
     resolution (DLAA) has 129600 tiles: a list goes as rows of groups, the row count
     kept by the append (d3d12_tile_header, shader/path_tracer.h). */

void d3d12_tile_header(uint32_t *header, uint32_t row_groups)
{
    for (int list = 0; list < PT_TILE_LISTS; list++) {
        uint32_t *a = header + PT_TILE_HDR_ARGS + list * 4;
        a[0] = 0;               // the tile count
        a[1] = row_groups;      // the DISPATCH: row_groups x rows x 1
        a[2] = 0;
        a[3] = 1;
    }
}

static D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES before,
                                                 D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// path_tracer.c's tile_lists_reset: the header and empty masks over the lists' head,
// ahead of the primary rays. Both buffers decay to COMMON between command lists, so
// the copies promote them.
static bool tile_lists_reset(recorder_t *r)
{
    const d3d12_passes_t *p = r->p;
    if (!p->tile_buffer || !p->tile_reset || !p->tile_args || !p->tile_dispatch ||
        p->tile_reset_size < PT_TILE_HDR_WORDS * sizeof(uint32_t))
        return false;
    ID3D12GraphicsCommandList4_CopyBufferRegion(p->cmd, p->tile_buffer, 0, p->tile_reset, 0, p->tile_reset_size);
    const D3D12_RESOURCE_BARRIER b = transition_barrier(p->tile_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12GraphicsCommandList4_ResourceBarrier(p->cmd, 1, &b);
    r->tile_args_read = false;
    return true;
}

// `pass` with `push`, dispatched by the DISPATCH at word `args_word` of the lists,
// copied out to tile_args first (above)
static void execute_from_lists(recorder_t *r, d3d12_pass_t pass, const pt_push_t *push, uint32_t args_word)
{
    const d3d12_passes_t *p = r->p;
    D3D12_RESOURCE_BARRIER b[2];
    UINT n = 0;
    b[n++] = transition_barrier(p->tile_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (r->tile_args_read)
        b[n++] = transition_barrier(p->tile_args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList4_ResourceBarrier(p->cmd, n, b);
    ID3D12GraphicsCommandList4_CopyBufferRegion(p->cmd, p->tile_args, 0, p->tile_buffer,
                                                (uint64_t)args_word * sizeof(uint32_t),
                                                sizeof(D3D12_DISPATCH_ARGUMENTS));
    b[0] = transition_barrier(p->tile_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    b[1] = transition_barrier(p->tile_args, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ID3D12GraphicsCommandList4_ResourceBarrier(p->cmd, 2, b);
    r->tile_args_read = true;

    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)p->cmd;
    ID3D12GraphicsCommandList4_SetPipelineState(p->cmd, p->psos[pass]);
    use_layout(r, D3D12_PL_PATH_TRACER);
    d3d12_compute_push_constants(gl, D3D12_PL_PATH_TRACER, push, sizeof(*push));
    // (none of reflect_refract, direct_lighting and indirect_lighting reads gl_NumWorkGroups, the root
    // constant d3d12_compute_dispatch sets and a DISPATCH argument cannot)
    ID3D12GraphicsCommandList4_ExecuteIndirect(p->cmd, p->tile_dispatch, 1, p->tile_args, 0, NULL, 0);
    d3d12_barrier_all(p->cmd, p->enhanced_barriers);
}

// path_tracer.c's dispatch_rays_indirect: bounce `bounce` over list `bounce`, adding
// to the next list
static void trace_tiles(recorder_t *r, d3d12_pass_t pass, int bounce)
{
    pt_push_t push = { 0 };
    push.gpu_index = -1;
    push.bounce_index = bounce;
    push.tile_read_list = (uint32_t)bounce + 1;
    push.tile_append_list = bounce + 1 < PT_TILE_LISTS ? (uint32_t)bounce + 2 : 0;
    execute_from_lists(r, pass, &push, PT_TILE_HDR_ARGS + (uint32_t)bounce * 4 + 1);
}

/* PATH LISTS (pt_bounce_cull, pt_sparse, shader/constants.h; path_tracer.c's
   dispatch_rays_paths): a lighting pass over path list `list` - for indirect lighting
   bounce `bounce` > 0 list `bounce`, the pixels whose path the bounce before continued;
   for direct lighting and the first bounce under pt_sparse PT_PATH_LIST_SPARSE - filling
   list `append` (0: none). Without pt_sparse bounce 0 covers the grid and fills list 1
   (trace_grid_append). */
static void trace_paths(recorder_t *r, d3d12_pass_t pass, int bounce, uint32_t list, uint32_t append)
{
    pt_push_t push = { 0 };
    push.gpu_index = -1;
    push.bounce_index = bounce;
    push.tile_read_list = list;
    push.tile_append_list = append;
    execute_from_lists(r, pass, &push, PT_PATH_HDR(list) + 1);
}

// vkpt_pt_trace_reflections: over the bounce's tile list when the view has them
static void reflect_refract(recorder_t *r, const d3d12_passes_frame_t *f, d3d12_pass_t pass, int bounce, bool tiles)
{
    if (tiles && bounce < PT_TILE_LISTS)
        trace_tiles(r, pass, bounce);
    else
        trace(r, pass, bounce, f->field_width, f->render_height);
}

/* ------------------------------------------------------------------------- */
/* NVIDIA FLOW (vkpt/flow.c)                                                  */
/* ------------------------------------------------------------------------- */

// a pass of `layout`, x * y * z groups, no push constants
static void flow_pass(recorder_t *r, d3d12_pass_t pass, d3d12_pipeline_layout_id_t layout, uint32_t x, uint32_t y,
                      uint32_t z)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)r->p->cmd;
    ID3D12GraphicsCommandList4_SetPipelineState(r->p->cmd, r->p->psos[pass]);
    use_layout(r, layout);
    d3d12_compute_dispatch(gl, layout, x, y, z);
    d3d12_barrier_all(r->p->cmd, r->p->enhanced_barriers);
}

void d3d12_record_flow(const d3d12_passes_t *p, const d3d12_flow_passes_t *f)
{
    recorder_t r = { p, -1 };
    const bool occupancy = f->occupancy && f->occupancy_blocks && p->zero_buffer;
    const bool lights = f->light_sums && f->light_sums_readback && f->light_blocks;

    // vkpt submits these two right after Flow's own work, inside the profiler's Flow
    // row; here they are timed apart and added to it (gpu_timers.c)
    if (occupancy || lights)
        mark(&r, D3D12_PROFILER_FLOW_PASSES, PROFILER_START);

    // flow_occupancy_dispatch: the group bitset is only ever ORed into, so it starts
    // each frame clear (vkCmdFillBuffer; the copies promote the buffer to COPY_DEST)
    if (occupancy) {
        zero_range(p->cmd, f->occupancy, f->occupancy_offset + (uint64_t)f->occupancy_capacity * 16,
                   f->group_bitset_size, p->zero_buffer);
        const D3D12_RESOURCE_BARRIER b = transition_barrier(f->occupancy, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList4_ResourceBarrier(p->cmd, 1, &b);
        const uint32_t blocks = f->occupancy_blocks < f->occupancy_capacity ? f->occupancy_blocks
                                                                            : f->occupancy_capacity;
        flow_pass(&r, D3D12_PASS_FLOW_OCCUPANCY, D3D12_PL_UBO_TEXTURES, blocks, 1, 1);
    }

    // flow_lights_dispatch, and its sums where the host reads them (vkpt's buffer is
    // host-visible; a READBACK buffer cannot be a UAV). Explicit, so the copy has a
    // state to leave from; only flow_lights.comp touches the buffer.
    if (lights) {
        D3D12_RESOURCE_BARRIER b = transition_barrier(f->light_sums, D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList4_ResourceBarrier(p->cmd, 1, &b);
        flow_pass(&r, D3D12_PASS_FLOW_LIGHTS, D3D12_PL_UBO_TEXTURES, f->light_blocks, 1, 1);
        b = transition_barrier(f->light_sums, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList4_ResourceBarrier(p->cmd, 1, &b);
        ID3D12GraphicsCommandList4_CopyBufferRegion(p->cmd, f->light_sums_readback, f->light_sums_offset,
                                                    f->light_sums, f->light_sums_offset, f->light_sums_size);
    }
    if (occupancy || lights)
        mark(&r, D3D12_PROFILER_FLOW_PASSES, PROFILER_STOP);

    // vkpt_flow_ambient: the probes, from this slot's TLAS (built ahead of this)
    if (p->flow_env_sets[0] && f->env_groups[0] && f->env_groups[1] && f->env_groups[2]) {
        mark(&r, PROFILER_FLOW_SMOKE_LIGHTING, PROFILER_START);
        flow_pass(&r, D3D12_PASS_FLOW_AMBIENT, D3D12_PL_FLOW_ENV, f->env_groups[0], f->env_groups[1],
                  f->env_groups[2]);
        mark(&r, PROFILER_FLOW_SMOKE_LIGHTING, PROFILER_STOP);
    }
}

void d3d12_record_shadow_map(ID3D12GraphicsCommandList4 *cmd, ID3D12PipelineState *pso,
                             D3D12_CPU_DESCRIPTOR_HANDLE layer, uint32_t size, bool clear,
                             const d3d12_shadow_draw_t *draws, uint32_t num_draws)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cmd;
    if (clear)
        ID3D12GraphicsCommandList4_ClearDepthStencilView(cmd, layer, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, NULL);
    if (!num_draws || !pso)
        return;

    ID3D12GraphicsCommandList4_SetPipelineState(cmd, pso);
    d3d12_graphics_begin(gl, D3D12_PL_SHADOW_MAP);
    const D3D12_VIEWPORT vp = { 0.f, 0.f, (float)size, (float)size, 0.f, 1.f };
    const D3D12_RECT sc = { 0, 0, (LONG)size, (LONG)size };
    ID3D12GraphicsCommandList4_RSSetViewports(cmd, 1, &vp);
    ID3D12GraphicsCommandList4_RSSetScissorRects(cmd, 1, &sc);
    ID3D12GraphicsCommandList4_OMSetRenderTargets(cmd, 0, NULL, FALSE, &layer);
    ID3D12GraphicsCommandList4_IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (uint32_t i = 0; i < num_draws; i++) {
        const d3d12_shadow_draw_t *d = &draws[i];
        if (!d->count || !d->vertices.SizeInBytes)
            continue;
        d3d12_graphics_push_constants(gl, D3D12_PL_SHADOW_MAP, d->mvp, sizeof(d->mvp));
        ID3D12GraphicsCommandList4_IASetVertexBuffers(cmd, 0, 1, &d->vertices);
        ID3D12GraphicsCommandList4_DrawInstanced(cmd, d->count, 1, d->first, 0);
    }
}

void d3d12_record_passes(const d3d12_passes_t *p, const d3d12_passes_frame_t *f)
{
    recorder_t r = { p, -1 };
    const bool fog = f->god_rays && p->god_rays_sets[0];

    // R_RenderFrame_RTX's trace list: with tile lists this view (TILE COMPACTION
    // above), the primary rays fill list 0 and the bounces walk them
    mark(&r, PROFILER_PRIMARY_RAYS, PROFILER_START);
    const bool tiles = tile_lists_reset(&r);
    trace_grid_append(&r, f->flow_split ? D3D12_PASS_PRIMARY_RAYS_FLOW_SPLIT : D3D12_PASS_PRIMARY_RAYS, 0,
                      f->field_width, f->render_height, 2, tiles ? 1 : 0);
    mark(&r, PROFILER_PRIMARY_RAYS, PROFILER_STOP);
    // pt_flow_split_pass: the Flow march over what the primary rays handed it (the
    // barrier after them orders the hand-off), before the fog's march writes ASVGF_COLOR
    if (f->flow_split) {
        mark(&r, PROFILER_FLOW_MARCH, PROFILER_START);
        trace_grid(&r, D3D12_PASS_FLOW_MARCH, 0, f->field_width, f->render_height, 2);
        mark(&r, PROFILER_FLOW_MARCH, PROFILER_STOP);
    }
    // the fog's march over the primary surfaces, then the froxel grid the filter reads
    if (fog) {
        mark(&r, PROFILER_GOD_RAYS, PROFILER_START);
        if (f->god_rays_march)
            god_rays_march(&r, f, 0);
        mark(&r, PROFILER_GOD_RAYS, PROFILER_STOP);
    }
    if (fog && f->froxel) {
        mark(&r, PROFILER_FOG_FROXEL, PROFILER_START);
        froxel(&r, f);
        mark(&r, PROFILER_FOG_FROXEL, PROFILER_STOP);
    }
    const d3d12_pass_t rr_first = f->flow_reflect ? D3D12_PASS_REFLECT_REFRACT_FIRST_FLOW
                                                  : D3D12_PASS_REFLECT_REFRACT_FIRST;
    const d3d12_pass_t rr_second = f->flow_reflect ? D3D12_PASS_REFLECT_REFRACT_SECOND_FLOW
                                                   : D3D12_PASS_REFLECT_REFRACT_SECOND;
    if (f->reflect_refract > 0) {
        mark(&r, PROFILER_REFLECT_REFRACT_1, PROFILER_START);
        reflect_refract(&r, f, rr_first, 0, tiles);
        mark(&r, PROFILER_REFLECT_REFRACT_1, PROFILER_STOP);
    }
    // ...and over what the first bounce saw, then the filter
    if (fog) {
        if (f->reflect_refract > 0 && f->god_rays_march) {
            mark(&r, PROFILER_GOD_RAYS_REFLECT_REFRACT, PROFILER_START);
            god_rays_march(&r, f, 1);
            mark(&r, PROFILER_GOD_RAYS_REFLECT_REFRACT, PROFILER_STOP);
        }
        mark(&r, PROFILER_GOD_RAYS_FILTER, PROFILER_START);
        god_rays_filter(&r, f);
        mark(&r, PROFILER_GOD_RAYS_FILTER, PROFILER_STOP);
    }
    if (f->reflect_refract > 1) {
        mark(&r, PROFILER_REFLECT_REFRACT_2, PROFILER_START);
        for (int bounce = 1; bounce < f->reflect_refract; bounce++)
            reflect_refract(&r, f, rr_second, bounce, tiles);
        mark(&r, PROFILER_REFLECT_REFRACT_2, PROFILER_STOP);
    }
    if (f->enable_denoiser) {
        mark(&r, PROFILER_ASVGF_GRADIENT_REPROJECT, PROFILER_START);
        pass(&r, D3D12_PASS_GRADIENT_REPROJECT, NULL, 0, (f->slice_width + 23) / 24, (f->render_height + 23) / 24);
        mark(&r, PROFILER_ASVGF_GRADIENT_REPROJECT, PROFILER_STOP);
    }
    // needs the primary surfaces, and must come before the first bounce, which
    // overwrites SHADING_POSITION when there is a second
    if (f->sharc && p->sharc_buffers[0] && p->sharc_buffers[1] && p->sharc_buffers[2])
        sharc_update(&r, f);
    trace_lighting(&r, f, tiles);

    // ...and its post list
    mark(&r, PROFILER_ASVGF_FULL, PROFILER_START);
    if (f->enable_denoiser) {
        asvgf_filter(&r, f, f->num_bounce_rays >= 0.5f);
    } else {
        mark(&r, PROFILER_COMPOSITING, PROFILER_START);
        pass(&r, D3D12_PASS_COMPOSITING, NULL, 0, (f->slice_width + 15) / 16, (f->render_height + 15) / 16);
        mark(&r, PROFILER_COMPOSITING, PROFILER_STOP);
    }
    mark(&r, PROFILER_ASVGF_FULL, PROFILER_STOP);

    // taa_dispatch_size: the output plus asvgf_taau's 8 pixel zero border
    // wherever the image has room for it
    uint32_t tw = f->taa_output_width, th = f->taa_output_height;
    if (tw < f->taa_image_width)
        tw += 8;
    if (th < f->taa_image_height)
        th += 8;

    // the render; with TAA folded in (DLSS only), also TAA's grid, so its zero
    // border is written as asvgf_taau would have
    uint32_t iw = f->render_width, ih = f->render_height;
    if (f->taa_folded) {
        iw = iw > tw ? iw : tw;
        ih = ih > th ? ih : th;
    }
    mark(&r, PROFILER_INTERLEAVE, PROFILER_START);
    pass(&r, D3D12_PASS_INTERLEAVE, NULL, 0, (iw + 15) / 16, (ih + 15) / 16);
    mark(&r, PROFILER_INTERLEAVE, PROFILER_STOP);

    // (an empty span when folded, as vkpt_taa's)
    mark(&r, PROFILER_ASVGF_TAA, PROFILER_START);
    if (!f->taa_folded)
        pass(&r, D3D12_PASS_TAAU, NULL, 0, (tw + 15) / 16, (th + 15) / 16);
    mark(&r, PROFILER_ASVGF_TAA, PROFILER_STOP);

    // vkpt's DLSS arm: DLSSApply (NGX), then R_EndFrame's bloom and tone mapping on
    // DLSS_OUTPUT at the output size
    uint32_t pw = f->taa_output_width, ph = f->taa_output_height;
    if (f->dlss) {
        if (f->dlss_bypass) {
            // photo mode (vkpt's accumulation_bypasses_dlss): the accumulated frame
            // where the DLSS arm reads it - vkpt_blit_accumulated_to_dlss_output, as
            // postfx_copy.comp's third mode
            const int32_t push[3] = { (int32_t)f->output_width, (int32_t)f->output_height, 2 };
            pass(&r, D3D12_PASS_POSTFX_COPY, push, sizeof(push), (f->output_width + 15) / 16,
                 (f->output_height + 15) / 16);
        } else if (p->upscale) {
            mark(&r, PROFILER_DLSS, PROFILER_START);
            p->upscale(p->upscale_ctx, p->cmd);
            after_ngx(&r);
            mark(&r, PROFILER_DLSS, PROFILER_STOP);
        }
        pw = f->output_width;
        ph = f->output_height;
        // vkpt's FRAME_TIME ends with DLSS: what follows is in its R_EndFrame
        mark(&r, PROFILER_FRAME_TIME, PROFILER_STOP);
    }

    // R_EndFrame's motion blur and chromatic aberration, between the resolve (or the
    // upscale) and bloom, each into MOTION_BLUR and copied back over the frame;
    // motion blur's copy is bloom's downscale when that is next (the DLSS arm)
    const bool blur_to_bloom = f->motion_blur && f->motion_blur_to_bloom && f->bloom && f->dlss;
    if (f->motion_blur) {
        mark(&r, PROFILER_MOTION_BLUR, PROFILER_START);
        pass(&r, D3D12_PASS_MOTION_BLUR, f->motion_blur_push, sizeof(f->motion_blur_push), (pw + 15) / 16,
             (ph + 15) / 16);
        if (!blur_to_bloom)
            postfx_copy(&r, f->dlss, pw, ph);
        mark(&r, PROFILER_MOTION_BLUR, PROFILER_STOP);
    }
    if (f->chromatic_aberration) {
        mark(&r, PROFILER_CHROMATIC_ABERRATION, PROFILER_START);
        pass(&r, D3D12_PASS_CHROMATIC_ABERRATION, f->chromatic_aberration_push, sizeof(f->chromatic_aberration_push),
             (pw + 15) / 16, (ph + 15) / 16);
        postfx_copy(&r, f->dlss, pw, ph);
        mark(&r, PROFILER_CHROMATIC_ABERRATION, PROFILER_STOP);
    }
    if (f->bloom) {
        mark(&r, PROFILER_BLOOM, PROFILER_START);
        bloom(&r, f, pw, ph, blur_to_bloom);
        mark(&r, PROFILER_BLOOM, PROFILER_STOP);
    }
    if (f->tone_mapping) {
        mark(&r, PROFILER_TONE_MAPPING, PROFILER_START);
        tone_mapping(&r, f, pw, ph);
        mark(&r, PROFILER_TONE_MAPPING, PROFILER_STOP);
    }
    // R_EndFrame_RTX's DLSS 5 (DLSS5Apply), on the tone-mapped frame at the output size:
    // its feature (made or remade first), the frame encoded for the model, the model's
    // answer into MOTION_BLUR, resolved back into DLSS_OUTPUT
    if (f->dlss5 && p->neural) {
        const uint32_t gx = (pw + 15) / 16, gy = (ph + 15) / 16;
        const bool made = p->neural(p->upscale_ctx, p->cmd, 0);
        after_ngx(&r);
        if (made) {
            pass(&r, D3D12_PASS_DLSS5_CONVERT, f->dlss5_push[0], sizeof(f->dlss5_push[0]), gx, gy);
            const bool evaluated = p->neural(p->upscale_ctx, p->cmd, 1);
            after_ngx(&r);
            if (evaluated)
                pass(&r, D3D12_PASS_DLSS5_CONVERT, f->dlss5_push[1], sizeof(f->dlss5_push[1]), gx, gy);
        }
    }
    // ...and without DLSS, FRAME_TIME holds the whole post list
    if (!f->dlss)
        mark(&r, PROFILER_FRAME_TIME, PROFILER_STOP);
}
