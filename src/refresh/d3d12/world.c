/*
THE 3D VIEW (phase 2): the world's GPU side.

frontend.c runs vkpt's CPU half - the BSP mesh at map load, then every frame
vkpt_prepare_frame's UBO, instance list and light buffer - and hands this file
plain data (d3d12pt.h). What it does with it:

  MAP LOAD    d3d12_world_load. The world buffer vkpt's shaders read as
              primitive_buffers[VERTEX_BUFFER_WORLD] - vkpt's buf_world layout,
              VboPrimitive[] then the positions, without the BLAS memory, which
              is a pool here - and the static BLASes, one per model_geometry_t
              (the five world classes and each brush model), built once in the
              upload batch. The GPU is idle on return.

  MODELS      d3d12_world_model_upload, from R_BeginFrame (frontend.c walks
              vkpt's model slots as vkpt_vertex_buffer_upload_models does):
              each model's VBO - vkpt's layout, its BLAS memory a pool here -
              and a static model's BLASes, built in the upload batch. The
              commit waits for the GPU and points primitive_buffers[] at them.

  A FRAME     d3d12_world_render, recorded into the frame list after
              R_BeginFrame's clear:
                * the light buffer, the instance buffer and the IQM matrices,
                  copied from this frame slot's staging (only the light and
                  IQM ranges vkpt says changed); the UBO is a CBV straight out
                  of the upload heap
                * vkpt_instance_geometry: instance_geometry.comp writes the
                  animated models into the instanced buffers, and
                  animate_materials.comp steps the world's texture animation
                * the particles, beams and sprites, copied from this slot's
                  slice of transparency.c's frame
                * the blood droplets' changed regions of blood.c's shadow copy,
                  through this slot's staging (upload_blood)
                * vkpt_pt_create_all_dynamic's BLASes - the model sections and
                  explosions over the instanced positions, the particles' and
                  sprites' quads, the beams' boxes - which frontend.c planned
                  in this frame slot's memory and added to the instance list
                * the TLAS, from vkpt's instance list as it is - a DXR instance
                  desc is byte for byte vkpt's instance
                * when vkpt says the sky needs it: the physical sky (sky.c) -
                  the terrain shadow map, physical_sky(_space).comp into the
                  envmap and the sun colour, sky_buffer_resolve.comp
                * primary_rays.rgen, ray query, into the G-buffer
                * vkpt_pt_trace_lighting's passes: direct lighting (its
                  caustics variant under pt_caustics), the indirect bounces,
                  ReSTIR GI; A-SVGF's gradient reprojection before them when
                  the denoiser is on
                * vkpt's post chain: A-SVGF (flt_enable) or plain compositing,
                  the checkerboard interleave, TAA, tone mapping
                * TAA_OUTPUT on screen through film_grain.frag (a nearest blit,
                  grain off), as vkpt_final_blit_simple. `d3d12_show <image>`
                  shows a G-buffer image instead, through the same blit with
                  TAA_OUTPUT's texture binding pointed at it
                * the readback buffer, copied to this slot's READBACK heap copy
                  for vkpt_readback next time round
                * the radiance cache (pt_sharc): trained and resolved after
                  the primary rays, before the lighting (d3d12_record_passes)
                * bloom (bloom.c's three passes, between TAA and tone mapping)
                * DLSS Super Resolution and Ray Reconstruction (frontend.c's
                  d3d12_frontend_dlss_record, ngx.c), then bloom and tone
                  mapping on DLSS_OUTPUT at the output size
                * the fog (god_rays.c): the sun's shadow map (shadow_map.c's
                  draws, in two parts - d3d12_world_render says why), the march
                  after the primary rays and after the first reflection bounce,
                  the froxel grid between them (its volumes made the first frame
                  it is wanted), the filter compositing it into PT_TRANSPARENT
              Not yet (ROADMAP.md): frame generation, motion blur.

  THE SKY     The tables precomputed_sky.c loads (DDS, sky.c's parser) and the
              images physical_sky.c makes, owned here. The preset's tables come
              through d3d12_world_sky_load_preset (vkpt's load_sky_preset hook),
              the terrain grid once from frontend.c, R_SetSky's cube through
              d3d12_world_upload_envmap (the upload_envmap hook). Each waits
              for the GPU and rewrites the textures sets' sky bindings.

SYNCHRONISATION. The G-buffer images are simultaneous-access textures and the
buffers are buffers: neither ever needs a transition between shader passes,
only a barrier ordering one pass's writes before the next's accesses - vkpt's
BARRIER_COMPUTE. With enhanced barriers that is one global barrier; without,
a UAV barrier on everything (passes.c's d3d12_barrier_all).
*/

#include "d3d12pt.h"
#include "passes.h"
#include "sky.h"
#include "system/system.h"

#include "../vkpt/shader/constants.h"
#include "../vkpt/shader/global_ubo.h"
#include "../vkpt/shader/global_textures.h"
#include "../vkpt/shader/vertex_buffer.h"
#include "../vkpt/shader/froxel_shared.h"

typedef char d3d12_instance_desc_is_vk_instance[(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) == 64) ? 1 : -1];

#define ALIGN_UP(x, a)  (((x) + (uint64_t)(a) - 1) & ~((uint64_t)(a) - 1))

// one frame slot's upload buffer: the UBO (a CBV: 256-byte aligned), the
// instance buffer's staging, the TLAS instance descs (16-byte aligned)
#define UPLOAD_UBO_OFFSET           0
#define UPLOAD_UBO_SIZE             ALIGN_UP(sizeof(QVKUniformBuffer_t), 256)
#define UPLOAD_INSTANCE_OFFSET      (UPLOAD_UBO_OFFSET + UPLOAD_UBO_SIZE)
#define UPLOAD_TLAS_OFFSET          ALIGN_UP(UPLOAD_INSTANCE_OFFSET + sizeof(InstanceBuffer), 256)
#define UPLOAD_SIZE                 (UPLOAD_TLAS_OFFSET + (uint64_t)MAX_TLAS_INSTANCES * sizeof(D3D12_RAYTRACING_INSTANCE_DESC))
// ...and with a partitioned TLAS route, its op list after them (ptlas_stage_size)
#define UPLOAD_PTLAS_OFFSET         ALIGN_UP(UPLOAD_SIZE, 256)

// the static BLASes are always built for tracing (vertex_buffer.c's); the
// TLAS and the per-frame BLASes as pt_tlas_fast_trace / pt_blas_fast_trace say
#define BLAS_BUILD_FLAGS            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
#define TLAS_FAST_TRACE             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
#define TLAS_FAST_BUILD             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD

// final_blit_pc.glsl's FinalBlitPushConstants (draw.c's FinalBlit_PushConstants_t)
typedef struct {
    int32_t  source, src_width, src_height, out_width, out_height, hdr;
    float    hdr_white_nits;
    uint32_t seed;
    float    grain_intensity, grain_sigma_px;
    int32_t  grain_radius;
    int32_t  crt_mode, crt_grid_width, crt_grid_height;
    float    crt_warp_x, crt_warp_y, crt_corner_radius_px, crt_hard_pix;
    float    crt_scan_hard_dark, crt_scan_hard_bright, crt_mask_strength;
    int32_t  crt_mask_pitch;
    float    crt_glow, crt_vignette;
} blit_push_t;
// final_blit_pc.glsl's push constants, the size frontend.c sends (no C11 here: a negative array fails)
typedef char blit_push_size_check[sizeof(blit_push_t) == D3D12_BLIT_PUSH_WORDS * 4 ? 1 : -1];

#define CRT_SHADER_OFF      0   // draw.c's crt_mode values
#define CRT_SHADER_GRID     1   // ...240 or 480 lines, through crt_downsample.comp

typedef struct {
    ID3D12Resource  *upload;        // UBO, instance buffer staging, TLAS instances
    uint8_t         *upload_map;
    // TILE COMPACTION's reset copy: this view's header, then the masks' zeros
    ID3D12Resource  *tile_reset;
    uint8_t         *tile_reset_map;
    ID3D12Resource  *staging[D3D12_NUM_WORLD_STAGING];
    uint8_t         *staging_map[D3D12_NUM_WORLD_STAGING];
    d3d12_as_pool_t tlas_pool;
    D3D12_GPU_VIRTUAL_ADDRESS tlas, tlas_effects;   // TLAS_INDEX_GEOMETRY, TLAS_INDEX_EFFECTS
    ID3D12Resource  *tlas_scratch;                  // the geometry TLAS's, then the effects TLAS's
    // pt_ptlas: the geometry TLAS as a partitioned one, at the same address (an AS
    // SRV is an address); what this slot's holds, against which the next is diffed
    ptlas_t         ptlas;
    d3d12_set_t     ubo_set, rt_set;
    d3d12_set_t     god_rays_set;   // the fog's: this slot's TLAS, slot i's side of the volumes' ping-pong
    // the frame's particles, beams and sprites (vkpt's transparency host buffer slice)
    ID3D12Resource  *tr_staging;
    uint8_t         *tr_staging_map;
    // the blood droplets' changed regions: VboPrimitive[], positions (blood.c's staging)
    ID3D12Resource  *blood_staging[2];
    uint8_t         *blood_staging_map[2];
    uint32_t        blood_capacity;                 // primitives
    // this slot's dynamic BLASes: their memory, and this frame's plan
    d3d12_dynamic_as_t      dyn;
    d3d12_dynamic_blas_t    dyn_blases[D3D12_NUM_DYN];
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS dyn_flags;
} world_frame_t;

// a model slot (vkpt's model_vertex_data[i]): its VBO, a static model's BLASes
typedef struct {
    ID3D12Resource  *buffer;
    uint64_t        primitives_size;
    d3d12_as_pool_t blas_pool;
    bool            dirty;          // the descriptor changes at the commit
    uint32_t        smap_stamp;     // = w.smap_stamp: in this frame's shadow map barriers
} world_model_t;

// god_rays.c's froxel volumes: the scatter, integrated and sky-part volumes by frame
// slot (slot i writes its own and samples the other's as history), the ReSTIR
// reservoir pair, and the sky exposure grid
enum {
    FROXEL_SCATTER,
    FROXEL_INTEGRATED   = FROXEL_SCATTER + 2,
    FROXEL_RESERVOIR    = FROXEL_INTEGRATED + 2,
    FROXEL_SKY          = FROXEL_RESERVOIR + 2,
    FROXEL_SKY_EXPOSURE = FROXEL_SKY + 2,
    NUM_FROXEL_VOLUMES
};

static struct {
    bool                initialized;
    world_frame_t       frame[D3D12_FRAMES_IN_FLIGHT];
    d3d12_as_sizes_t    tlas_sizes;
    // THE PARTITIONED TLAS (accel.h): NVAPI's or the preview runtime's, or none; the
    // geometry TLAS's scratch, big enough for either kind
    d3d12_ptlas_route_t ptlas_route;
    uint64_t            geometry_scratch;
    // CLUSTERED DYNAMIC GEOMETRY (pt_clusters, vkpt/clusters.h): on for this
    // renderer's life (its pipelines were made for it); off for good after NVAPI
    // refused a build
    bool                clusters;
    bool                clusters_failed;

    ID3D12Resource      *light_buffer;      // LightBuffer
    ID3D12Resource      *instance_buffer;   // InstanceBuffer
    ID3D12Resource      *readback_buffer;   // ReadbackBuffer, written by the shaders
    ID3D12Resource      *tonemap_buffer;    // ToneMappingBuffer: the histogram, the curve
    ID3D12Resource      *zero_buffer;       // D3D12_ZERO_BUFFER_SIZE zeros, the source of buffer clears
    ID3D12Resource      *blue_noise;        // textures.c's TEX_BLUE_NOISE (not owned)
    bool                tm_reset;           // clear the tone mapping state next frame
    // the radiance cache (pt_sharc): vertex_buffer.c's keys, accumulators and
    // resolved entries, ~68 MB; cleared before the first frame that runs it
    ID3D12Resource      *sharc[3];
    uint64_t            sharc_size[3];
    bool                sharc_dirty;

    // the models
    world_model_t       models[D3D12_MAX_MODELS];
    ID3D12Resource      *model_scratch[D3D12_MAX_MODELS];  // their BLASes', until the commit
    int                 num_model_scratch;
    ID3D12Resource      *iqm_buffer;        // IqmMatrixBuffer: the skeletal models' bones
    // the last frame's blood regions' memcpy into the staging (pt_blood_stats)
    uint64_t            blood_copy_usec;
    // a frame's blood regions went nowhere (d3d12_world_take_blood_lost)
    bool                blood_lost;

    // the instanced buffers (vkpt's buf_primitive_instanced, buf_positions_instanced):
    // instance_geometry.comp's output, VboPrimitive[] and 9 floats per triangle
    ID3D12Resource      *prims_instanced, *positions_instanced;
    uint32_t            primbuf_size;
    // the transparency buffers (transparency.c's), D3D12_TR_INDICES' included
    ID3D12Resource      *tr[D3D12_TR_NUM_STREAMS + 1];
    uint64_t            tr_capacity[D3D12_TR_NUM_STREAMS + 1];
    uint64_t            tr_frame_capacity;

    // the map
    ID3D12Resource      *world_buffer;
    d3d12_as_pool_t     blas_pool;
    // light_stats_bufers[]: shadow ray hits and misses per light, cluster and
    // orientation; this frame writes [frame % 3], the light sampling reads the
    // previous two
    ID3D12Resource      *light_stats[NUM_LIGHT_STATS_BUFFERS];
    uint64_t            light_stats_size;
    bool                light_stats_fresh;  // a new map: clear all three

    // the G-buffer
    ID3D12Resource      *images[NUM_VKPT_IMAGES];
    d3d12_extents_t     extents;
    bool                images_made;
    d3d12_set_t         textures[2];        // even, odd frames (vkpt's desc_set_textures_even/odd)
    d3d12_set_t         display[2];         // the same, TAA_OUTPUT's texture = the image shown
    int                 display_image;      // what display[] shows: pt_dlss_debug's view, else show_image
    int                 show_image;         // d3d12_show's choice
    d3d12_set_t         vertex_buffer_set;
    d3d12_set_t         tiles_set;
    // TILE COMPACTION (pt_tile_compaction): the lists (vkpt's buf_tiles, sized by the
    // header's layout and remade when it changes), the 12 bytes a bounce's DISPATCH is
    // copied to, and its command signature; each frame slot has its reset copy
    ID3D12Resource      *tiles;
    uint64_t            tiles_size;
    uint32_t            tiles_reset_size;
    uint64_t            tiles_failed_size;  // a size that could not be made (not retried)
    ID3D12Resource      *tile_args;
    ID3D12CommandSignature *tile_dispatch;

    // vkpt's pipelines (path_tracer.c, asvgf.c, tone_mapping.c), one per
    // passes.c pass
    ID3D12PipelineState *passes[D3D12_NUM_PASSES];
    // SER (pt_ser): indirect lighting as ray generation shaders, [0 without the reorder
    // (pt_ser 2), 1 with it (pt_ser 1)][first bounce, later], made when first asked
    // for; their raygen records, one 64-byte slot each, in an upload buffer
    ID3D12StateObject   *ser[2][2];
    bool                ser_failed[2];
    ID3D12Resource      *ser_table;
    uint8_t             *ser_table_map;
    ID3D12PipelineState *blit[2];       // [d3d.hdr]: film_grain.frag into the frame target's format
    ID3D12PipelineState *blit_crt[2];   // ...crt.frag (pt_crt; NULL: film_grain.frag, no CRT)
    // film grain and the CRT (the last drawn frame's, d3d12_world_frame_t's blit_effects):
    // its push constants without a source, for record_blit
    bool                blit_effects;
    blit_push_t         blit_fx;

    // the physical sky (physical_sky.c, precomputed_sky.c), sky.c's shapes
    ID3D12Resource      *sky_envmap;        // TEX_PHYSICAL_SKY, IMG_PHYSICAL_SKY
    ID3D12Resource      *sky_skybox;        // TEX_ENVMAP, R_SetSky's
    ID3D12Resource      *sky_textures[7];   // the DDS tables, sky_texture_files' order
    D3D12_SHADER_RESOURCE_VIEW_DESC sky_srvs[7];
    ID3D12Resource      *sky_shadowmap;     // TEX_TERRAIN_SHADOWMAP
    ID3D12DescriptorHeap *sky_dsv_heap;     // its one DSV
    ID3D12Resource      *sky_grid;          // the terrain: vertices, then indices
    D3D12_VERTEX_BUFFER_VIEW sky_grid_vertices;
    D3D12_INDEX_BUFFER_VIEW sky_grid_indices;
    uint32_t            sky_grid_index_count;
    ID3D12Resource      *sun_color_buffer;  // SunColorBuffer: SUN_COLOR_BUFFER, and SUN_COLOR_UBO
    ID3D12Resource      *sky_atmosphere;    // the preset's AtmosphereParameters
    d3d12_set_t         sky_set;
    ID3D12PipelineState *sky_pso, *sky_space_pso, *sky_resolve_pso, *sky_shadowmap_pso;

    // the fog (god_rays.c, shadow_map.c): the sun's shadow map - D32, two layers, the
    // opaque world and models then the water - with a DSV per layer; the sky exposure
    // bake's solid mask; the froxel volumes once the grid is wanted
    ID3D12Resource      *smap;
    ID3D12DescriptorHeap *smap_dsv_heap;
    uint32_t            smap_stamp;
    uint64_t            world_positions_offset;     // where the world buffer's positions start
    ID3D12Resource      *sky_exposure_mask;
    ID3D12Resource      *froxel[NUM_FROXEL_VOLUMES];
    bool                fog_ready, froxel_made, froxel_failed;

    cvar_t              *show;              // d3d12_show: TAA_OUTPUT, or a G-buffer image
    bool                show_warned;

    // the frame d3d12_world_render last drew, for frame generation's passes after it
    // (d3d12_world_dlssg_convert, d3d12_world_blit_generated): its slot, its texture
    // set's parity, the output size. last_drawn is false until a frame is drawn.
    bool                last_drawn;
    int                 last_slot, last_odd;
    uint32_t            last_output_width, last_output_height;
} w;

static const size_t staging_sizes[D3D12_NUM_WORLD_STAGING] = {
    [D3D12_WORLD_STAGING_LIGHTS]    = sizeof(LightBuffer),
    [D3D12_WORLD_STAGING_IQM]       = sizeof(IqmMatrixBuffer),
    [D3D12_WORLD_STAGING_READBACK]  = sizeof(ReadbackBuffer),
};

/* ------------------------------------------------------------------------- */

static void transition(ID3D12GraphicsCommandList4 *cmd, ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList4_ResourceBarrier(cmd, 1, &b);
}

// `zero`: write zeros through the mapping. A committed buffer already starts zeroed
// (D3D12_HEAP_FLAG_NONE), so only a caller that wants the pages touched up front
// needs it; the blood staging (hundreds of MB, only written regions ever read) does not.
static ID3D12Resource *mapped_buffer_ex(uint64_t size, D3D12_HEAP_TYPE heap, const char *name, uint8_t **map,
                                        bool zero)
{
    ID3D12Resource *b = d3d12_create_buffer(size, heap, D3D12_RESOURCE_FLAG_NONE, name);
    if (!b)
        return NULL;
    // an upload buffer is never read back; a readback one is read whole
    D3D12_RANGE none = { 0, 0 };
    void *p = NULL;
    if (!D3D_CHECK(ID3D12Resource_Map(b, 0, heap == D3D12_HEAP_TYPE_UPLOAD ? &none : NULL, &p))) {
        D3D12_RELEASE(b);
        return NULL;
    }
    if (zero)
        memset(p, 0, size);
    *map = p;
    return b;
}

static ID3D12Resource *mapped_buffer(uint64_t size, D3D12_HEAP_TYPE heap, const char *name, uint8_t **map)
{
    return mapped_buffer_ex(size, heap, name, map, true);
}

/* ------------------------------------------------------------------------- */
/* PIPELINES                                                                  */
/* ------------------------------------------------------------------------- */

static ID3D12PipelineState *compute_pso(d3d12_pipeline_layout_id_t layout, const char *shader)
{
    size_t size = 0;
    void *code = d3d12_load_shader(shader, &size);
    if (!code)
        return NULL;
    ID3D12PipelineState *pso = d3d12_create_compute_pso(layout, code, size, NULL);
    FS_FreeFile(code);
    if (!pso)
        Com_EPrintf("D3D12: no pipeline for %s against the %s layout\n", shader, d3d12_pipeline_layouts[layout].name);
    else
        d3d12_set_name((ID3D12Object *)pso, shader);
    return pso;
}

/* SHADER EXECUTION REORDERING (pt_ser), Matt's A/B: the indirect lighting bounces built
   again as ray generation shaders (spv2dxil --raygen / --ser), dispatched with
   DispatchRays over the same threads (passes.c's trace_raygen).
     0  the compute pass (default)
     1  ray generation + dx::MaybeReorderThread, the lanes regrouped by the hit's material
        (indirect_lighting.rgen's d3d12_ser_reorder)
     2  ray generation without the reorder: what the dispatch path costs by itself
   All three write the same pixels. Needs shader model 6.9 - the Agility runtime (device.c).
   Read every frame; the state objects are made the first time a mode is asked for, so
   pt_ser 0 costs nothing. profiler 1 shows the indirect lighting's GPU time. */
static cvar_t *pt_ser;

static bool ser_ready(int mode)
{
    const int v = mode == 1 ? 1 : 0;
    if (w.ser[v][0])
        return true;
    if (w.ser_failed[v])
        return false;
    w.ser_failed[v] = true;     // until made

    if (d3d.caps.shader_model < D3D_SHADER_MODEL_6_9) {
        Com_WPrintf("D3D12: pt_ser needs shader model 6.9 - the Agility SDK runtime (this device has %d.%d)\n",
                    d3d.caps.shader_model >> 4, d3d.caps.shader_model & 0xf);
        return false;
    }
    if (!w.ser_table) {
        // four records, 64 bytes apart (D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT)
        w.ser_table = d3d12_create_buffer(4 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_FLAG_NONE, "SER raygen records");
        D3D12_RANGE none = { 0, 0 };
        void *map = NULL;
        if (!w.ser_table || !D3D_CHECK(ID3D12Resource_Map(w.ser_table, 0, &none, &map))) {
            D3D12_RELEASE(w.ser_table);
            return false;
        }
        w.ser_table_map = map;
    }

    for (int k = 0; k < 2; k++) {
        const char *name = va("indirect_lighting.rgen.query%s.%s", k ? ".spec_0_1" : "", v ? "ser" : "raygen");
        size_t size = 0;
        void *dxil = d3d12_load_shader(name, &size);
        if (!dxil)
            return false;
        wchar_t wname[64];
        swprintf(wname, 64, L"%hs", name);
        w.ser[v][k] = d3d12_create_raygen_object(D3D12_PL_PATH_TRACER, dxil, size,
                                                 w.ser_table_map + (v * 2 + k) * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
                                                 wname);
        FS_FreeFile(dxil);
        if (!w.ser[v][k]) {
            Com_EPrintf("D3D12: pt_ser %d: no ray generation state object for %s\n", mode, name);
            D3D12_RELEASE(w.ser[v][0]);
            return false;
        }
    }
    w.ser_failed[v] = false;
    Com_Printf("D3D12: pt_ser %d - indirect lighting as a ray generation shader, %s\n", mode,
               v ? "with MaybeReorderThread by material" : "without the reorder");
    return true;
}

// this frame's: the ray generation objects pt_ser asks for, or none
static void ser_select(d3d12_passes_t *p)
{
    const int mode = pt_ser ? pt_ser->integer : 0;
    if ((mode != 1 && mode != 2) || !ser_ready(mode))
        return;
    const int v = mode == 1 ? 1 : 0;
    const D3D12_GPU_VIRTUAL_ADDRESS table = ID3D12Resource_GetGPUVirtualAddress(w.ser_table);
    for (int k = 0; k < 2; k++) {
        p->raygen[k] = w.ser[v][k];
        p->raygen_records[k] = table + (uint64_t)(v * 2 + k) * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    }
}

// film_grain.frag (or crt.frag) over the window into `rtv`, reading `sets` (the UBO's,
// then the textures or the d3d12_show display set) as `in` says; the swapchain's format
// and the UI's white (final_blit_params) are filled in here. `effects`: with the last
// frame's grain and CRT (w.blit_fx) - vkpt's final_blit_effects, for a picture
static void record_blit(ID3D12GraphicsCommandList4 *cmd, const d3d12_set_t *const *sets, const blit_push_t *in,
                        D3D12_CPU_DESCRIPTOR_HANDLE rtv, bool effects)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cmd;
    blit_push_t pc = *in;
    if (effects) {
        pc = w.blit_fx;
        pc.source = in->source;
        pc.src_width = in->src_width;
        pc.src_height = in->src_height;
        pc.out_width = in->out_width;
        pc.out_height = in->out_height;
        pc.seed = in->seed;
    }
    pc.hdr = d3d.hdr ? 1 : 0;   // final_blit_output's clamp to tm_hdr_peak_nits
    float saturation, video_gamma;
    d3d12_frontend_ui_hdr_params(&pc.hdr_white_nits, &saturation, &video_gamma);
    pc.hdr_white_nits = max(pc.hdr_white_nits, 1.f);

    ID3D12PipelineState *crt = pc.crt_mode != CRT_SHADER_OFF ? w.blit_crt[d3d.hdr ? 1 : 0] : NULL;
    // draw.c's crt_downsample: the tube's 240 or 480 lines of signal into MOTION_BLUR
    // for crt.frag, from the source (after everything that wrote either: the frame's
    // passes and FG's end in a barrier)
    if (crt && pc.crt_mode == CRT_SHADER_GRID && w.passes[D3D12_PASS_CRT_DOWNSAMPLE]) {
        ID3D12GraphicsCommandList4_SetPipelineState(cmd, w.passes[D3D12_PASS_CRT_DOWNSAMPLE]);
        d3d12_compute_begin(gl, D3D12_PL_UBO_TEXTURES);
        d3d12_compute_bind_sets(gl, D3D12_PL_UBO_TEXTURES, sets);
        d3d12_compute_push_constants(gl, D3D12_PL_UBO_TEXTURES, &pc, sizeof(pc));
        d3d12_compute_dispatch(gl, D3D12_PL_UBO_TEXTURES, ((uint32_t)pc.crt_grid_width + 7) / 8,
                               ((uint32_t)pc.crt_grid_height + 7) / 8, 1);
        d3d12_barrier_all(cmd, d3d.caps.enhanced_barriers);
    }

    ID3D12GraphicsCommandList4_SetPipelineState(cmd, crt ? crt : w.blit[d3d.hdr ? 1 : 0]);
    d3d12_graphics_begin(gl, D3D12_PL_UBO_TEXTURES);
    d3d12_graphics_bind_sets(gl, D3D12_PL_UBO_TEXTURES, sets);
    d3d12_graphics_push_constants(gl, D3D12_PL_UBO_TEXTURES, &pc, sizeof(pc));

    D3D12_VIEWPORT vp = { 0.f, 0.f, (float)d3d.width, (float)d3d.height, 0.f, 1.f };
    D3D12_RECT sc = { 0, 0, d3d.width, d3d.height };
    ID3D12GraphicsCommandList4_RSSetViewports(cmd, 1, &vp);
    ID3D12GraphicsCommandList4_RSSetScissorRects(cmd, 1, &sc);
    ID3D12GraphicsCommandList4_OMSetRenderTargets(cmd, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList4_IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList4_DrawInstanced(cmd, 4, 1, 0, 0);
}

static ID3D12PipelineState *blit_pso(const char *vertex, const char *fragment, DXGI_FORMAT format)
{
    size_t vs_size = 0, ps_size = 0;
    void *vs = d3d12_load_shader(vertex, &vs_size);
    void *ps = d3d12_load_shader(fragment, &ps_size);
    ID3D12PipelineState *pso = NULL;
    if (vs && ps) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
        pd.pRootSignature = d3d12_pipeline_layouts[D3D12_PL_UBO_TEXTURES].root_signature;
        pd.VS.pShaderBytecode = vs;
        pd.VS.BytecodeLength = vs_size;
        pd.PS.pShaderBytecode = ps;
        pd.PS.BytecodeLength = ps_size;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // a full-screen strip; spv2dxil's y flip mirrors its winding
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = format;
        pd.SampleDesc.Count = 1;
        if (!D3D_CHECK(ID3D12Device5_CreateGraphicsPipelineState(d3d.device, &pd, &IID_ID3D12PipelineState,
                                                                (void **)&pso)))
            pso = NULL;
        else
            d3d12_set_name((ID3D12Object *)pso, fragment);
    }
    if (vs) FS_FreeFile(vs);
    if (ps) FS_FreeFile(ps);
    return pso;
}

/* ------------------------------------------------------------------------- */
/* THE G-BUFFER                                                               */
/* ------------------------------------------------------------------------- */

static int find_image(const char *name)
{
    for (int i = 0; i < NUM_VKPT_IMAGES; i++)
        if (!Q_stricmp(d3d12_image_name(i), name))
            return i;
    return -1;
}

// a textures set as vkpt's: every image at its storage and its sampled binding,
// the four samplers and the sampler table; `taa_output_shows` >= 0 puts that
// image at TAA_OUTPUT's sampled binding instead (the display sets)
static void write_textures_set(const d3d12_set_t *set, int odd, int taa_output_shows)
{
    for (int i = 0; i < NUM_VKPT_IMAGES; i++) {
        int shown = d3d12_image_at_binding(i, odd);
        const unsigned b = d3d12_image_binding(i);
        d3d12_set_write_uav_image(set, BINDING_OFFSET_IMAGES + b, 0, w.images[shown], NULL);
        if (i == VKPT_IMG_TAA_OUTPUT && taa_output_shows >= 0)
            shown = d3d12_image_at_binding(taa_output_shows, odd);
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(d3d12_image_sampler(shown), &sd);
        d3d12_set_write_srv_image(set, BINDING_OFFSET_TEXTURES + b, 0, w.images[shown], NULL, &sd);
    }
    for (int s = 0; s < D3D12_NUM_GLOBAL_SAMPLERS; s++) {
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(s, &sd);
        d3d12_set_write_sampler(set, D3D12_BINDING_GLOBAL_SAMPLERS, s, &sd);
    }
    d3d12_set_write_buffer(set, D3D12_BINDING_GLOBAL_SAMPLER_INDEX, 0, d3d12_sampler_index_resource(), 0, MAX_RIMAGES);
    if (w.blue_noise) {
        // read with texelFetch; the sampler is vkpt's tex_sampler for form's sake
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(D3D12_GLOBAL_SAMPLER_DEFAULT, &sd);
        d3d12_set_write_srv_image(set, BINDING_OFFSET_BLUE_NOISE, 0, w.blue_noise, NULL, &sd);
    }
}

static void write_display_sets(void)
{
    for (int odd = 0; odd < 2; odd++)
        write_textures_set(&w.display[odd], odd, w.display_image);
}

static void destroy_images(void)
{
    for (int i = 0; i < NUM_VKPT_IMAGES; i++)
        D3D12_DEFER_RELEASE(w.images[i]);
    w.images_made = false;
}

ID3D12Resource *d3d12_world_image(int image)
{
    return w.images_made && image >= 0 && image < NUM_VKPT_IMAGES ? w.images[image] : NULL;
}

const d3d12_set_t *d3d12_world_textures_set(int odd)
{
    return w.initialized ? &w.textures[odd & 1] : NULL;
}

// vkpt/textures.c's image_is_active (vkpt.h; world.c does not include it)
bool vkpt_screen_image_active(int index, uint32_t profile);

// an image's size as made at these extents: what this configuration never writes
// (the A-SVGF chain under Ray Reconstruction or flt_enable 0, photo mode's image
// without it) is 1x1, so every binding stays valid, as vkpt_create_images makes it
static void image_made_extent(int image, const d3d12_extents_t *e, uint32_t *width, uint32_t *height)
{
    d3d12_image_extent(image, e, width, height);
    if (!vkpt_screen_image_active(image, e->profile))
        *width = *height = 1;
}

// (Re)makes the G-buffer at these extents. The textures sets are rewritten in
// place, so the GPU must be idle first: frames in flight read them.
static bool ensure_images(const d3d12_extents_t *e)
{
    if (w.images_made && !memcmp(e, &w.extents, sizeof(*e)))
        return true;

    const uint64_t t0 = Sys_Microseconds();
    d3d12_wait_idle();
    // The old set goes NOW, not through the deferred list: the frames in flight
    // are done (the wait), and this frame has not used it - this is the first
    // thing d3d12_world_render does. Deferred, it lived until this frame's fence,
    // after the new set was made, so both were alive at once: at 4K with split
    // fields 4.6 + 9.3 GB going into photo mode (7680x2160), past a 16 GB card's
    // budget, and the new images were placed in system memory - the long pause
    // into photo mode and the low frame rate after leaving it (11-06).
    for (int i = 0; i < NUM_VKPT_IMAGES; i++)
        D3D12_RELEASE(w.images[i]);
    w.images_made = false;

    uint64_t total = 0;
    for (int i = 0; i < NUM_VKPT_IMAGES; i++) {
        uint32_t iw, ih;
        image_made_extent(i, e, &iw, &ih);
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { 0 };
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = iw;
        rd.Height = ih;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = d3d12_image_format(i);
        rd.SampleDesc.Count = 1;
        // storage + sampled with no layout changes, as vkpt's (VK_IMAGE_LAYOUT_GENERAL)
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                            D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource,
                                                            (void **)&w.images[i]))) {
            Com_EPrintf("D3D12: G-buffer image %s (%ux%u) failed\n", d3d12_image_name(i), iw, ih);
            destroy_images();
            return false;
        }
        d3d12_set_name((ID3D12Object *)w.images[i], d3d12_image_name(i));
        D3D12_RESOURCE_ALLOCATION_INFO ai;
        ID3D12Device5_GetResourceAllocationInfo(d3d.device, &ai, 0, 1, &rd);
        total += ai.SizeInBytes;
    }

    for (int odd = 0; odd < 2; odd++)
        write_textures_set(&w.textures[odd], odd, -1);
    write_display_sets();

    w.extents = *e;
    w.images_made = true;

    // every rebuild (photo mode in and out, a resolution or DLSS change): its size,
    // its time, and the video memory against the budget Windows gives the process -
    // over it, the driver moves allocations to system memory and frames slow down
    const unsigned ms = (unsigned)((Sys_Microseconds() - t0) / 1000);
    DXGI_QUERY_VIDEO_MEMORY_INFO local = { 0 }, shared = { 0 };
    if (d3d.adapter && SUCCEEDED(IDXGIAdapter4_QueryVideoMemoryInfo(d3d.adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                                                     &local))) {
        IDXGIAdapter4_QueryVideoMemoryInfo(d3d.adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
        Com_Printf("D3D12: G-buffer %ux%u (output %ux%u), %.2f GB in %u ms; video memory %.2f of %.2f GB budget"
                   " (%.2f GB in system memory)\n", e->screen_width, e->screen_height, e->unscaled_width,
                   e->unscaled_height, total / 1073741824.0, ms, local.CurrentUsage / 1073741824.0,
                   local.Budget / 1073741824.0, shared.CurrentUsage / 1073741824.0);
        if (local.Budget && local.CurrentUsage > local.Budget)
            Com_WPrintf("D3D12: over the video memory budget - Windows moves memory to system RAM and the frame "
                        "rate drops\n");
    } else {
        Com_Printf("D3D12: G-buffer %ux%u (output %ux%u), %.2f GB in %u ms\n", e->screen_width, e->screen_height,
                   e->unscaled_width, e->unscaled_height, total / 1073741824.0, ms);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* TILE COMPACTION                                                            */
/* ------------------------------------------------------------------------- */

static void destroy_tiles(void)
{
    D3D12_RELEASE(w.tiles);
    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++) {
        D3D12_RELEASE(w.frame[f].tile_reset);
        w.frame[f].tile_reset_map = NULL;
    }
    w.tiles_size = 0;
    w.tiles_reset_size = 0;
}

// vkpt_pt_create_tiles: the lists for the layout this view's header names (the
// screen images'), and each frame slot's reset copy, remade with the GPU idle when
// it changes. Without them the bounces cover the whole grid, as before.
static bool ensure_tiles(const uint32_t *header)
{
    const uint64_t size = ((uint64_t)header[PT_TILE_HDR_DATA] +
                           (uint64_t)PT_TILE_LISTS * header[PT_TILE_HDR_DATA_STRIDE]) * sizeof(uint32_t);
    const uint32_t reset_size = header[PT_TILE_HDR_DATA] * (uint32_t)sizeof(uint32_t);
    if (w.tiles && size == w.tiles_size && reset_size == w.tiles_reset_size)
        return true;
    if (!w.tile_args || !w.tile_dispatch || size == w.tiles_failed_size ||
        reset_size < PT_TILE_HDR_WORDS * sizeof(uint32_t))
        return false;

    // the frames in flight read the lists through the tiles set
    d3d12_wait_idle();
    destroy_tiles();
    w.tiles = d3d12_create_buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  "pt tiles");
    bool ok = w.tiles != NULL;
    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT && ok; f++) {
        // (mapped_buffer zeroes it, and only the header is ever written: the masks stay zero)
        w.frame[f].tile_reset = mapped_buffer(reset_size, D3D12_HEAP_TYPE_UPLOAD, "pt tiles reset",
                                              &w.frame[f].tile_reset_map);
        ok = w.frame[f].tile_reset != NULL;
    }
    if (!ok) {
        destroy_tiles();
        w.tiles_failed_size = size;
        Com_WPrintf("D3D12: no tile lists (%.1f KB) - the reflect/refract bounces cover the whole screen\n",
                    size / 1024.0);
        return false;
    }
    d3d12_set_write_buffer(&w.tiles_set, 0, 0, w.tiles, 0, size);
    w.tiles_size = size;
    w.tiles_reset_size = reset_size;
    w.tiles_failed_size = 0;
    Com_DPrintf("D3D12: tile lists %.1f KB\n", size / 1024.0);
    return true;
}

/* ------------------------------------------------------------------------- */
/* THE SKY                                                                    */
/* ------------------------------------------------------------------------- */

// precomputed_sky.c's DDS tables: the preset's three (named for its planet),
// then the four SkyInitializeDataGPU loads once
enum {
    SKY_TRANSMITTANCE, SKY_SCATTERING, SKY_IRRADIANCE,
    SKY_CLOUDS, SKY_TERRAIN_ALBEDO, SKY_TERRAIN_NORMALS, SKY_TERRAIN_DEPTH,
    NUM_SKY_TEXTURES
};
typedef char sky_texture_slots[(NUM_SKY_TEXTURES == (int)(sizeof(w.sky_textures) / sizeof(w.sky_textures[0]))) ? 1 : -1];

static const struct {
    const char          *file;              // %s: the preset's planet
    unsigned            binding;
    D3D12_SRV_DIMENSION shape;              // the shader's type: the null view of a table not loaded
} sky_texture_files[NUM_SKY_TEXTURES] = {
    { "env/transmittance_%s.dds",   BINDING_OFFSET_SKY_TRANSMITTANCE,   D3D12_SRV_DIMENSION_TEXTURE2D },
    { "env/inscatter_%s.dds",       BINDING_OFFSET_SKY_SCATTERING,      D3D12_SRV_DIMENSION_TEXTURE3D },
    { "env/irradiance_%s.dds",      BINDING_OFFSET_SKY_IRRADIANCE,      D3D12_SRV_DIMENSION_TEXTURE2D },
    { "env/clouds.dds",             BINDING_OFFSET_SKY_CLOUDS,          D3D12_SRV_DIMENSION_TEXTURE3D },
    { "env/terrain_albedo.dds",     BINDING_OFFSET_TERRAIN_ALBEDO,      D3D12_SRV_DIMENSION_TEXTURECUBE },
    { "env/terrain_normal.dds",     BINDING_OFFSET_TERRAIN_NORMALS,     D3D12_SRV_DIMENSION_TEXTURECUBE },
    { "env/terrain_depth.dds",      BINDING_OFFSET_TERRAIN_DEPTH,       D3D12_SRV_DIMENSION_TEXTURECUBE },
};

#define SUN_COLOR_SIZE  ALIGN_UP(sizeof(SunColorBuffer), 256)     // a CBV too

// a texture's state change on the upload list, after its copies
static void upload_transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    ID3D12GraphicsCommandList *up = d3d12_upload_list();
    if (!up)
        return;
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(up, 1, &b);
}

// precomputed_sky.c's LoadImageFromDDS. NULL when the file is missing or not
// one it reads: the shaders read zeros, as from Vulkan's image never made.
static ID3D12Resource *load_sky_dds(const char *path, D3D12_SHADER_RESOURCE_VIEW_DESC *sv)
{
    void *file = NULL;
    const int len = FS_LoadFile(path, &file);
    if (!file) {
        Com_EPrintf("D3D12: couldn't read %s\n", path);
        return NULL;
    }

    ID3D12Resource *r = NULL;
    d3d12_dds_t dds;
    char error[160];
    if (!d3d12_dds_parse(file, (size_t)len, &dds, error, sizeof(error))) {
        Com_EPrintf("D3D12: %s: %s\n", path, error);
    } else {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd;
        d3d12_dds_resource_desc(&dds, &rd);
        if (D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                           (void **)&r))) {
            d3d12_set_name((ID3D12Object *)r, path);
            // a 3D image is one subresource, a cube's faces one each
            const uint32_t subresources = dds.depth > 1 ? 1 : dds.layers;
            const size_t each = dds.size / subresources;
            for (uint32_t i = 0; i < subresources; i++)
                d3d12_upload_subresource(r, i, dds.pixels + each * i, (int)dds.width, (int)dds.height,
                                         (int)dds.bytes_per_pixel);
            upload_transition(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            d3d12_dds_srv_desc(&dds, sv);
        } else {
            r = NULL;
        }
    }
    FS_FreeFile(file);
    return r;
}

static D3D12_CPU_DESCRIPTOR_HANDLE sky_dsv(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w.sky_dsv_heap, &h);
    return h;
}

static void null_srv(D3D12_SRV_DIMENSION shape, D3D12_SHADER_RESOURCE_VIEW_DESC *sv)
{
    memset(sv, 0, sizeof(*sv));
    sv->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv->ViewDimension = shape;
    sv->Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (shape == D3D12_SRV_DIMENSION_TEXTURE3D)
        sv->Texture3D.MipLevels = 1;
    else if (shape == D3D12_SRV_DIMENSION_TEXTURECUBE)
        sv->TextureCube.MipLevels = 1;
    else
        sv->Texture2D.MipLevels = 1;
}

// the sky's bindings of a textures set, each its image or a null view of its shape,
// through vkpt's tex_sampler as all of them are on Vulkan
static void write_sky_textures_set(const d3d12_set_t *set)
{
    D3D12_SAMPLER_DESC sd;
    d3d12_global_sampler_desc(D3D12_GLOBAL_SAMPLER_DEFAULT, &sd);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;

    for (int i = 0; i < NUM_SKY_TEXTURES; i++) {
        if (w.sky_textures[i])
            sv = w.sky_srvs[i];
        else
            null_srv(sky_texture_files[i].shape, &sv);
        d3d12_set_write_srv_image(set, sky_texture_files[i].binding, 0, w.sky_textures[i], &sv, &sd);
    }

    d3d12_sky_envmap_srv_desc(&sv);
    d3d12_set_write_srv_image(set, BINDING_OFFSET_PHYSICAL_SKY, 0, w.sky_envmap, &sv, &sd);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv;
    d3d12_sky_envmap_uav_desc(&uv);
    d3d12_set_write_uav_image(set, BINDING_OFFSET_PHYSICAL_SKY_IMG, 0, w.sky_envmap, &uv);

    d3d12_sky_skybox_srv_desc(&sv);
    d3d12_set_write_srv_image(set, BINDING_OFFSET_ENVMAP, 0, w.sky_skybox, &sv, &sd);
    d3d12_sky_shadowmap_srv_desc(&sv);
    d3d12_set_write_srv_image(set, BINDING_OFFSET_TERRAIN_SHADOWMAP, 0, w.sky_shadowmap, &sv, &sd);
}

// every textures set, in place: the GPU must be idle (or the sets unused yet)
static void write_sky_sets(void)
{
    for (int i = 0; i < 2; i++) {
        write_sky_textures_set(&w.textures[i]);
        write_sky_textures_set(&w.display[i]);
    }
}

static ID3D12PipelineState *shadowmap_pso(void)
{
    size_t size = 0;
    void *vs = d3d12_load_shader("shadow_map.vert", &size);
    if (!vs)
        return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    D3D12_INPUT_ELEMENT_DESC element;
    d3d12_sky_shadowmap_pso_desc(&pd, &element, d3d12_pipeline_layouts[D3D12_PL_SHADOW_MAP].root_signature, vs, size);
    ID3D12PipelineState *pso = NULL;
    if (!D3D_CHECK(ID3D12Device5_CreateGraphicsPipelineState(d3d.device, &pd, &IID_ID3D12PipelineState, (void **)&pso)))
        pso = NULL;
    else
        d3d12_set_name((ID3D12Object *)pso, "shadow_map.vert");
    FS_FreeFile(vs);
    return pso;
}

// physical_sky.c's vkpt_physical_sky_initialize and precomputed_sky.c's
// SkyInitializeDataGPU: everything but the preset's tables and the grid,
// which frontend.c hands over
static bool sky_init(void)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd;

    d3d12_sky_envmap_desc(&rd);
    if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                        D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource,
                                                        (void **)&w.sky_envmap))) {
        w.sky_envmap = NULL;
        return false;
    }
    d3d12_set_name((ID3D12Object *)w.sky_envmap, "physical sky");

    D3D12_CLEAR_VALUE clear;
    d3d12_sky_shadowmap_desc(&rd, &clear);
    if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, &IID_ID3D12Resource,
                                                        (void **)&w.sky_shadowmap))) {
        w.sky_shadowmap = NULL;
        return false;
    }
    d3d12_set_name((ID3D12Object *)w.sky_shadowmap, "terrain shadow map");
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
    if (!D3D_CHECK(ID3D12Device5_CreateDescriptorHeap(d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                     (void **)&w.sky_dsv_heap))) {
        w.sky_dsv_heap = NULL;
        return false;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    d3d12_sky_shadowmap_dsv_desc(&dv);
    ID3D12Device5_CreateDepthStencilView(d3d.device, w.sky_shadowmap, &dv, sky_dsv());
    // far everywhere - nothing shadowed - until a grid is drawn, then kept
    // where d3d12_record_sky leaves it
    ID3D12GraphicsCommandList *up = d3d12_upload_list();
    if (!up)
        return false;
    ID3D12GraphicsCommandList_ClearDepthStencilView(up, sky_dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, NULL);
    upload_transition(w.sky_shadowmap, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    w.sun_color_buffer = d3d12_create_buffer(SUN_COLOR_SIZE, D3D12_HEAP_TYPE_DEFAULT,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "sun color");
    // zeros (committed memory) until a preset is loaded, as Vulkan's buffer
    w.sky_atmosphere = d3d12_create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                           "atmosphere parameters");
    if (!w.sun_color_buffer || !w.sky_atmosphere)
        return false;

    if (!d3d12_set_alloc(&w.sky_set, D3D12_SET_SKY, "sky"))
        return false;
    d3d12_set_write_cbv(&w.sky_set, 0, 0, ID3D12Resource_GetGPUVirtualAddress(w.sky_atmosphere), 256);
    d3d12_set_write_buffer(&w.vertex_buffer_set, SUN_COLOR_BUFFER_BINDING_IDX, 0, w.sun_color_buffer, 0,
                           sizeof(SunColorBuffer));
    d3d12_set_write_cbv(&w.vertex_buffer_set, SUN_COLOR_UBO_BINDING_IDX, 0,
                        ID3D12Resource_GetGPUVirtualAddress(w.sun_color_buffer), (uint32_t)SUN_COLOR_SIZE);

    for (int i = SKY_CLOUDS; i < NUM_SKY_TEXTURES; i++)
        w.sky_textures[i] = load_sky_dds(sky_texture_files[i].file, &w.sky_srvs[i]);

    w.sky_pso = compute_pso(D3D12_PL_PHYSICAL_SKY, "physical_sky.comp");
    w.sky_space_pso = compute_pso(D3D12_PL_PHYSICAL_SKY, "physical_sky_space.comp");
    w.sky_resolve_pso = compute_pso(D3D12_PL_UBO_VERTEX_BUFFER, "sky_buffer_resolve.comp");
    w.sky_shadowmap_pso = shadowmap_pso();
    if (!w.sky_pso || !w.sky_space_pso || !w.sky_resolve_pso || !w.sky_shadowmap_pso)
        return false;

    write_sky_sets();
    return true;
}

static void sky_shutdown(void)
{
    D3D12_RELEASE(w.sky_envmap);
    D3D12_RELEASE(w.sky_skybox);
    for (int i = 0; i < NUM_SKY_TEXTURES; i++)
        D3D12_RELEASE(w.sky_textures[i]);
    D3D12_RELEASE(w.sky_shadowmap);
    D3D12_RELEASE(w.sky_dsv_heap);
    D3D12_RELEASE(w.sky_grid);
    w.sky_grid_index_count = 0;
    D3D12_RELEASE(w.sun_color_buffer);
    D3D12_RELEASE(w.sky_atmosphere);
    D3D12_RELEASE(w.sky_pso);
    D3D12_RELEASE(w.sky_space_pso);
    D3D12_RELEASE(w.sky_resolve_pso);
    D3D12_RELEASE(w.sky_shadowmap_pso);
}

void d3d12_world_sky_load_preset(const void *atmosphere, size_t atmosphere_size, const char *planet)
{
    if (!w.initialized)
        return;
    // the tables are replaced under the textures sets (vkpt_physical_sky_load_preset waits too)
    d3d12_wait_idle();
    for (int i = SKY_TRANSMITTANCE; i <= SKY_IRRADIANCE; i++) {
        D3D12_RELEASE(w.sky_textures[i]);
        char path[MAX_QPATH];
        Q_snprintf(path, sizeof(path), sky_texture_files[i].file, planet);
        w.sky_textures[i] = load_sky_dds(path, &w.sky_srvs[i]);
    }
    if (atmosphere && atmosphere_size)
        d3d12_upload_buffer(w.sky_atmosphere, 0, atmosphere, min(atmosphere_size, (size_t)256));
    write_sky_sets();
}

void d3d12_world_sky_set_terrain(const float *vertices, uint32_t num_vertices, const uint32_t *indices,
                                 uint32_t num_indices)
{
    if (!w.initialized || !num_vertices || !num_indices)
        return;
    d3d12_wait_idle();
    D3D12_RELEASE(w.sky_grid);
    w.sky_grid_index_count = 0;

    const uint64_t vertex_bytes = (uint64_t)num_vertices * 3 * sizeof(float);
    const uint64_t index_offset = ALIGN_UP(vertex_bytes, 256);
    const uint64_t index_bytes = (uint64_t)num_indices * sizeof(uint32_t);
    w.sky_grid = d3d12_create_buffer(index_offset + index_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                     "terrain grid");
    if (!w.sky_grid)
        return;
    // decays to COMMON once the upload list is done; the draw's reads promote it
    d3d12_upload_buffer(w.sky_grid, 0, vertices, vertex_bytes);
    d3d12_upload_buffer(w.sky_grid, index_offset, indices, index_bytes);

    const D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(w.sky_grid);
    w.sky_grid_vertices.BufferLocation = va;
    w.sky_grid_vertices.SizeInBytes = (UINT)vertex_bytes;
    w.sky_grid_vertices.StrideInBytes = 3 * sizeof(float);
    w.sky_grid_indices.BufferLocation = va + index_offset;
    w.sky_grid_indices.SizeInBytes = (UINT)index_bytes;
    w.sky_grid_indices.Format = DXGI_FORMAT_R32_UINT;
    w.sky_grid_index_count = num_indices;
}

/* vkpt_physical_sky_read_envmap's D3D12 half: cpu_bench's copy of the physical sky's
   cube, laid out as Vulkan's (six w x h RGBA16F faces back to back, the same shaders
   write it). The bench starts from the console, between frames: through the upload
   list, then the GPU is waited for. The cube is a simultaneous-access texture, in
   COMMON between lists, so the copy promotes it to a copy source. */
uint16_t *d3d12_world_read_sky_envmap(int *width, int *height)
{
    if (!w.initialized || !w.sky_envmap)
        return NULL;
    D3D12_RESOURCE_DESC desc;
    ID3D12Resource_GetDesc(w.sky_envmap, &desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.DepthOrArraySize != 6 || desc.MipLevels != 1)
        return NULL;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[6];
    UINT rows[6];
    UINT64 row_sizes[6], total = 0;
    ID3D12Device5_GetCopyableFootprints(d3d.device, &desc, 0, 6, 0, fp, rows, row_sizes, &total);

    ID3D12Resource *readback = d3d12_create_buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                                   "sky readback");
    ID3D12GraphicsCommandList *cmd = readback ? d3d12_upload_list() : NULL;
    if (!cmd) {
        D3D12_RELEASE(readback);
        return NULL;
    }
    for (UINT i = 0; i < 6; i++) {
        D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
        to.pResource = readback;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = fp[i];
        from.pResource = w.sky_envmap;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = i;
        ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &to, 0, 0, 0, &from, NULL);
    }
    d3d12_upload_flush();
    d3d12_wait_idle();

    uint16_t *texels = NULL;
    void *m = NULL;
    D3D12_RANGE range = { 0, (SIZE_T)total };
    if (D3D_CHECK(ID3D12Resource_Map(readback, 0, &range, &m))) {
        const uint32_t fw = (uint32_t)desc.Width, fh = desc.Height;
        const size_t row = (size_t)fw * 4 * sizeof(uint16_t);
        texels = Z_Malloc(row * fh * 6);
        for (int i = 0; i < 6; i++)
            for (uint32_t y = 0; y < fh; y++)
                memcpy((byte *)texels + ((size_t)i * fh + y) * row,
                       (const byte *)m + fp[i].Offset + (size_t)y * fp[i].Footprint.RowPitch, row);
        D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Unmap(readback, 0, &none);
        *width = (int)fw;
        *height = (int)fh;
    }
    ID3D12Resource_Release(readback);
    return texels;
}

/* The FG debug tools' read (vkpt_fg_debug_frames): a region of a G-buffer image, as
   the sky's readback above - through the upload list, after the frame's list on the
   queue, then the GPU is waited for. The images are simultaneous-access textures in
   COMMON between lists, promoted to copy sources. */
bool d3d12_world_read_image(int image, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint16_t *out)
{
    ID3D12Resource *res = d3d12_world_image(image);
    if (!res || !width || !height)
        return false;
    D3D12_RESOURCE_DESC desc;
    ID3D12Resource_GetDesc(res, &desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || x + width > desc.Width || y + height > desc.Height)
        return false;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = { 0 };
    fp.Footprint.Format = desc.Format;
    fp.Footprint.Width = width;
    fp.Footprint.Height = height;
    fp.Footprint.Depth = 1;
    fp.Footprint.RowPitch = (UINT)ALIGN_UP((uint64_t)width * 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t total = (uint64_t)fp.Footprint.RowPitch * height;
    ID3D12Resource *readback = d3d12_create_buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                                   "image readback");
    ID3D12GraphicsCommandList *cmd = readback ? d3d12_upload_list() : NULL;
    if (!cmd) {
        D3D12_RELEASE(readback);
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
    to.pResource = readback;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = fp;
    from.pResource = res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    const D3D12_BOX box = { x, y, 0, x + width, y + height, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &to, 0, 0, 0, &from, &box);
    d3d12_upload_flush();
    d3d12_wait_idle();

    bool ok = false;
    void *m = NULL;
    D3D12_RANGE range = { 0, (SIZE_T)total };
    if (D3D_CHECK(ID3D12Resource_Map(readback, 0, &range, &m))) {
        const size_t row = (size_t)width * 8;
        for (uint32_t r = 0; r < height; r++)
            memcpy((byte *)out + r * row, (const byte *)m + (size_t)r * fp.Footprint.RowPitch, row);
        D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Unmap(readback, 0, &none);
        ok = true;
    }
    ID3D12Resource_Release(readback);
    return ok;
}

void d3d12_world_upload_envmap(int width, int height, const byte *data)
{
    if (!w.initialized || width <= 0 || height <= 0 || !data)
        return;
    // the textures sets are rewritten in place
    d3d12_wait_idle();
    D3D12_RELEASE(w.sky_skybox);

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd;
    d3d12_sky_skybox_desc((uint32_t)width, (uint32_t)height, &rd);
    if (D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                       (void **)&w.sky_skybox))) {
        d3d12_set_name((ID3D12Object *)w.sky_skybox, "skybox");
        const size_t face = (size_t)width * height * 4;
        for (int i = 0; i < 6; i++)
            d3d12_upload_subresource(w.sky_skybox, (uint32_t)i, data + face * i, width, height, 4);
        upload_transition(w.sky_skybox, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    } else {
        w.sky_skybox = NULL;
    }
    write_sky_sets();
}

/* ------------------------------------------------------------------------- */
/* INIT / SHUTDOWN                                                            */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* THE FOG                                                                    */
/* ------------------------------------------------------------------------- */

static D3D12_CPU_DESCRIPTOR_HANDLE smap_dsv(int layer)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w.smap_dsv_heap, &h);
    h.ptr += (SIZE_T)layer * ID3D12Device5_GetDescriptorHandleIncrementSize(d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return h;
}

static DXGI_FORMAT froxel_format(int v)
{
    // god_rays.c's: the reservoirs RGBA32F (a light index and a RIS weight), the sky
    // exposure packed R32UI, the rest RGBA16F
    if (v == FROXEL_RESERVOIR || v == FROXEL_RESERVOIR + 1)
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (v == FROXEL_SKY_EXPOSURE)
        return DXGI_FORMAT_R32_UINT;
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
}

static void froxel_extent(int v, uint32_t e[3])
{
    e[0] = v == FROXEL_SKY_EXPOSURE ? SKY_EXPOSURE_DIM_X : FROXEL_GRID_X;
    e[1] = v == FROXEL_SKY_EXPOSURE ? SKY_EXPOSURE_DIM_Y : FROXEL_GRID_Y;
    e[2] = v == FROXEL_SKY_EXPOSURE ? SKY_EXPOSURE_DIM_Z : FROXEL_GRID_Z;
}

// a volume's storage binding; a null view of its shape until the grid is made
static void froxel_uav(const d3d12_set_t *set, unsigned binding, int v)
{
    uint32_t e[3];
    froxel_extent(v, e);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = { 0 };
    uv.Format = froxel_format(v);
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uv.Texture3D.WSize = e[2];
    d3d12_set_write_uav_image(set, binding, 0, w.froxel[v], &uv);
}

// ...its sampled one, through god_rays.c's froxel_sampler: linear, and a transparent
// black border, so a history lookup off the volume reads as no history
static void froxel_srv(const d3d12_set_t *set, unsigned binding, int v)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.Format = froxel_format(v);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture3D.MipLevels = 1;
    D3D12_SAMPLER_DESC sd = { 0 };
    sd.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    d3d12_set_write_srv_image(set, binding, 0, w.froxel[v], &sv, &sd);
}

// god_rays.c's update_descriptor_set for slot i: the ping-pong - set i writes
// scatter and sky volume i and samples the other as their history
static void write_froxel_bindings(const d3d12_set_t *set, int i)
{
    const int o = 1 - i;
    froxel_uav(set, 2, FROXEL_SCATTER + i);
    froxel_srv(set, 3, FROXEL_SCATTER + o);
    froxel_uav(set, 4, FROXEL_INTEGRATED + i);
    froxel_srv(set, 5, FROXEL_INTEGRATED + i);
    froxel_uav(set, 6, FROXEL_RESERVOIR);
    froxel_uav(set, 7, FROXEL_RESERVOIR + 1);
    froxel_uav(set, 8, FROXEL_SKY + i);
    froxel_srv(set, 9, FROXEL_SKY + o);
    froxel_uav(set, 10, FROXEL_SKY_EXPOSURE);
}

// slot i's god rays set: the shadow map through vkpt's MIN-reduction sampler (white
// border: off the map is unshadowed), the slot's TLAS, the volumes, the mask
static void write_god_rays_set(int i)
{
    const d3d12_set_t *set = &w.frame[i].god_rays_set;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.ArraySize = 2;
    D3D12_SAMPLER_DESC sd = { 0 };
    sd.Filter = D3D12_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.f;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    d3d12_set_write_srv_image(set, 0, 0, w.smap, &sv, &sd);
    d3d12_set_write_as(set, 1, 0, w.frame[i].tlas);
    write_froxel_bindings(set, i);
    d3d12_set_write_buffer(set, 11, 0, w.sky_exposure_mask, 0, SKY_EXPOSURE_MASK_WORDS * sizeof(uint32_t));
}

// shadow_map.c's vkpt_shadow_map_initialize and god_rays.c's vkpt_initialize_god_rays:
// the map (SHADOWMAP_SIZE square, a layer for the opaque geometry and one for the
// water), far everywhere and kept in NON_PIXEL_SHADER_RESOURCE between the fog's
// frames; the mask; the sets
static bool fog_init(void)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = SHADOWMAP_SIZE;
    rd.Height = SHADOWMAP_SIZE;
    rd.DepthOrArraySize = 2;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = { DXGI_FORMAT_D32_FLOAT };
    clear.DepthStencil.Depth = 1.f;
    if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, &IID_ID3D12Resource,
                                                        (void **)&w.smap))) {
        w.smap = NULL;
        return false;
    }
    d3d12_set_name((ID3D12Object *)w.smap, "sun shadow map");

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2 };
    if (!D3D_CHECK(ID3D12Device5_CreateDescriptorHeap(d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                     (void **)&w.smap_dsv_heap))) {
        w.smap_dsv_heap = NULL;
        return false;
    }
    for (int i = 0; i < 2; i++) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = { 0 };
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.FirstArraySlice = (UINT)i;
        dv.Texture2DArray.ArraySize = 1;
        ID3D12Device5_CreateDepthStencilView(d3d.device, w.smap, &dv, smap_dsv(i));
    }
    ID3D12GraphicsCommandList *up = d3d12_upload_list();
    if (!up)
        return false;
    for (int i = 0; i < 2; i++)
        ID3D12GraphicsCommandList_ClearDepthStencilView(up, smap_dsv(i), D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, NULL);
    upload_transition(w.smap, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // zeros (committed memory) until a map's mask is uploaded; nothing reads it before
    w.sky_exposure_mask = d3d12_create_buffer(SKY_EXPOSURE_MASK_WORDS * sizeof(uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "sky exposure mask");
    if (!w.sky_exposure_mask)
        return false;

    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++) {
        if (!d3d12_set_alloc(&w.frame[f].god_rays_set, D3D12_SET_GOD_RAYS, "god rays"))
            return false;
        write_god_rays_set(f);
    }
    return true;
}

static void fog_shutdown(void)
{
    D3D12_RELEASE(w.smap);
    D3D12_RELEASE(w.smap_dsv_heap);
    D3D12_RELEASE(w.sky_exposure_mask);
    for (int v = 0; v < NUM_FROXEL_VOLUMES; v++)
        D3D12_RELEASE(w.froxel[v]);
    w.fog_ready = w.froxel_made = w.froxel_failed = false;
}

bool d3d12_world_fog_ready(void)
{
    return w.initialized && w.fog_ready;
}

// a volume zeroed by copies out of the zero buffer, as many slices at a time as it holds
static void zero_volume(ID3D12GraphicsCommandList *up, ID3D12Resource *volume)
{
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource_GetDesc(volume, &rd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size, total;
    ID3D12Device5_GetCopyableFootprints(d3d.device, &rd, 0, 1, 0, &fp, &rows, &row_size, &total);
    const uint64_t slice = (uint64_t)fp.Footprint.RowPitch * rows;
    const UINT step = (UINT)max(1, D3D12_ZERO_BUFFER_SIZE / slice);
    for (UINT z = 0; z < fp.Footprint.Depth; z += step) {
        D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
        to.pResource = volume;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = w.zero_buffer;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = fp;
        from.PlacedFootprint.Offset = 0;
        from.PlacedFootprint.Footprint.Depth = min(step, fp.Footprint.Depth - z);
        ID3D12GraphicsCommandList_CopyTextureRegion(up, &to, 0, 0, z, &from, NULL);
    }
}

bool d3d12_world_zero_volume(ID3D12Resource *volume)
{
    ID3D12GraphicsCommandList *up = w.initialized && volume ? d3d12_upload_list() : NULL;
    if (!up)
        return false;
    zero_volume(up, volume);
    return true;
}

bool d3d12_world_froxel_create(void)
{
    if (w.froxel_made)
        return true;
    if (!d3d12_world_fog_ready() || w.froxel_failed)
        return false;

    // storage and sampled with no state changes, as the G-buffer: simultaneous-access,
    // in COMMON (god_rays.c keeps its volumes in VK_IMAGE_LAYOUT_GENERAL)
    uint64_t total = 0;
    bool ok = true;
    for (int v = 0; v < NUM_FROXEL_VOLUMES && ok; v++) {
        uint32_t e[3];
        froxel_extent(v, e);
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { 0 };
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.Width = e[0];
        rd.Height = e[1];
        rd.DepthOrArraySize = (UINT16)e[2];
        rd.MipLevels = 1;
        rd.Format = froxel_format(v);
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        ok = D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                            D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource,
                                                            (void **)&w.froxel[v]));
        if (!ok) {
            w.froxel[v] = NULL;
            break;
        }
        d3d12_set_name((ID3D12Object *)w.froxel[v], v == FROXEL_SKY_EXPOSURE ? "sky exposure" : "froxel volume");
        D3D12_RESOURCE_ALLOCATION_INFO ai;
        ID3D12Device5_GetResourceAllocationInfo(d3d.device, &ai, 0, 1, &rd);
        total += ai.SizeInBytes;
    }

    // zeroed once, in the upload batch that runs ahead of this frame's list: the
    // first frame's history read comes before anything has written them
    ID3D12GraphicsCommandList *up = ok ? d3d12_upload_list() : NULL;
    if (!up) {
        Com_EPrintf("D3D12: no memory for the froxel grid - pt_fog_froxel stays off until a renderer restart\n");
        for (int v = 0; v < NUM_FROXEL_VOLUMES; v++)
            D3D12_RELEASE(w.froxel[v]);
        w.froxel_failed = true;
        return false;
    }
    for (int v = 0; v < NUM_FROXEL_VOLUMES; v++)
        zero_volume(up, w.froxel[v]);

    // frames in flight read the sets
    d3d12_wait_idle();
    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++)
        write_froxel_bindings(&w.frame[f].god_rays_set, f);
    w.froxel_made = true;
    Com_Printf("froxel grid: %d volumes of %dx%dx%d, %.1f MB of VRAM\n", NUM_FROXEL_VOLUMES, FROXEL_GRID_X,
               FROXEL_GRID_Y, FROXEL_GRID_Z, total / (1024.0 * 1024.0));
    return true;
}

void d3d12_world_sky_exposure_mask(const uint32_t *bits, uint32_t words)
{
    if (d3d12_world_fog_ready() && bits)
        d3d12_upload_buffer(w.sky_exposure_mask, 0, bits, (uint64_t)min(words, (uint32_t)SKY_EXPOSURE_MASK_WORDS) * 4);
}

// float3 positions from `offset` to the end of the buffer
static D3D12_VERTEX_BUFFER_VIEW positions_view(ID3D12Resource *buffer, uint64_t offset)
{
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource_GetDesc(buffer, &rd);
    D3D12_VERTEX_BUFFER_VIEW v;
    v.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buffer) + offset;
    v.SizeInBytes = offset < rd.Width ? (UINT)min(rd.Width - offset, (uint64_t)UINT_MAX) : 0;
    v.StrideInBytes = 3 * sizeof(float);
    return v;
}

static D3D12_RESOURCE_BARRIER state_barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

/* vkpt_shadow_map_render's first half, at the head of the frame's list: both layers
   cleared, the world's opaque run and the static instances into the first, the water
   into the second. Nothing earlier in the list has touched the world's or the models'
   buffers, so each goes COMMON -> VERTEX_AND_CONSTANT_BUFFER -> COMMON around the
   draws, and animate_materials and instance_geometry promote them afresh after. A
   read state cannot be promoted to their UNORDERED_ACCESS, which is why this is not
   drawn where vkpt draws it, after the BLAS builds. The map stays in DEPTH_WRITE for
   shadow_map_dynamic. */
static void shadow_map_static(ID3D12GraphicsCommandList4 *cmd, const d3d12_world_frame_t *frame)
{
    static D3D12_RESOURCE_BARRIER barriers[1 + D3D12_MAX_MODELS];
    static d3d12_shadow_draw_t draws[1 + MAX_MODEL_INSTANCES];
    uint32_t nb = 0, nd = 0;

    w.smap_stamp++;
    barriers[nb++] = state_barrier(w.world_buffer, D3D12_RESOURCE_STATE_COMMON,
                                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (frame->smap_opaque_count) {
        d3d12_shadow_draw_t *d = &draws[nd++];
        d->vertices = positions_view(w.world_buffer, w.world_positions_offset);
        d->first = frame->smap_opaque_first;
        d->count = frame->smap_opaque_count;
        memcpy(d->mvp, frame->shadowmap_viewproj, sizeof(d->mvp));
    }
    const uint32_t n = min(frame->smap_num_instances, (uint32_t)MAX_MODEL_INSTANCES);
    for (uint32_t i = 0; i < n; i++) {
        const d3d12_shadow_instance_t *si = &frame->smap_instances[i];
        ID3D12Resource *buffer = w.world_buffer;
        if (si->model >= 0) {
            if (si->model >= D3D12_MAX_MODELS || !w.models[si->model].buffer)
                continue;
            world_model_t *m = &w.models[si->model];
            buffer = m->buffer;
            if (m->smap_stamp != w.smap_stamp) {
                m->smap_stamp = w.smap_stamp;
                barriers[nb++] = state_barrier(buffer, D3D12_RESOURCE_STATE_COMMON,
                                               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            }
        }
        if (!si->prim_count)
            continue;
        d3d12_shadow_draw_t *d = &draws[nd++];
        d->vertices = positions_view(buffer, si->positions_offset);
        d->first = 0;
        d->count = si->prim_count * 3;
        memcpy(d->mvp, si->mvp, sizeof(d->mvp));
    }

    ID3D12GraphicsCommandList4_ResourceBarrier(cmd, nb, barriers);
    transition(cmd, w.smap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    d3d12_record_shadow_map(cmd, w.sky_shadowmap_pso, smap_dsv(0), SHADOWMAP_SIZE, true, draws, nd);

    d3d12_shadow_draw_t water;
    water.vertices = positions_view(w.world_buffer, w.world_positions_offset);
    water.first = frame->smap_water_first;
    water.count = frame->smap_water_count;
    memcpy(water.mvp, frame->shadowmap_viewproj, sizeof(water.mvp));
    d3d12_record_shadow_map(cmd, w.sky_shadowmap_pso, smap_dsv(1), SHADOWMAP_SIZE, true, &water,
                            water.count ? 1 : 0);

    for (uint32_t i = 0; i < nb; i++) {
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    }
    ID3D12GraphicsCommandList4_ResourceBarrier(cmd, nb, barriers);
}

// ...and its second: the animated models instance_geometry wrote this frame, into the
// first layer over what is there (a depth test does not care about the order). Then
// the map is the fog's to sample.
static void shadow_map_dynamic(ID3D12GraphicsCommandList4 *cmd, const d3d12_world_frame_t *frame)
{
    if (frame->smap_dynamic_count && w.positions_instanced) {
        d3d12_shadow_draw_t d;
        d.vertices = positions_view(w.positions_instanced, 0);
        d.first = 0;
        d.count = min(frame->smap_dynamic_count, w.primbuf_size * 3);
        memcpy(d.mvp, frame->shadowmap_viewproj, sizeof(d.mvp));
        d3d12_record_shadow_map(cmd, w.sky_shadowmap_pso, smap_dsv(0), SHADOWMAP_SIZE, false, &d, 1);
    }
    transition(cmd, w.smap, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool d3d12_world_init(void)
{
    memset(&w, 0, sizeof(w));
    pt_ser = Cvar_Get("pt_ser", "0", 0);
    // the renderer menu's SER row: ser_ready's first test
    Cvar_FullSet("pt_ser_available", d3d.caps.shader_model >= D3D_SHADER_MODEL_6_9 ? "1" : "0", CVAR_ROM, FROM_CODE);
    w.show = Cvar_Get("d3d12_show", "TAA_OUTPUT", 0);
    // survives a vid_restart with whatever was last chosen
    w.show_image = find_image(w.show->string);
    if (w.show_image < 0)
        w.show_image = VKPT_IMG_TAA_OUTPUT;
    w.show->modified = false;
    // pt_dlss_debug wins - update_display_image
    w.display_image = DLSSDebugImage(NULL);
    if (w.display_image < 0)
        w.display_image = w.show_image;

    // big enough for either of pt_tlas_fast_trace's builds
    d3d12_as_sizes_t trace_sizes, build_sizes;
    if (!d3d12_tlas_sizes(d3d.device, MAX_TLAS_INSTANCES, TLAS_FAST_TRACE, &trace_sizes) ||
        !d3d12_tlas_sizes(d3d.device, MAX_TLAS_INSTANCES, TLAS_FAST_BUILD, &build_sizes)) {
        Com_EPrintf("D3D12: no TLAS sizes from the driver\n");
        return false;
    }
    w.tlas_sizes.result = max(trace_sizes.result, build_sizes.result);
    w.tlas_sizes.scratch = max(trace_sizes.scratch, build_sizes.scratch);

    /* THE PARTITIONED TLAS (pt_ptlas): through NVAPI on the retail runtime, or the
       preview runtime's own. The geometry TLAS's memory and scratch hold either kind,
       so they are sized for the bigger, and the descriptors never change. */
    uint64_t geometry_result = w.tlas_sizes.result;
    w.geometry_scratch = w.tlas_sizes.scratch;
    w.ptlas_route = d3d12_ptlas_route(d3d.device);

    /* CLUSTERED DYNAMIC GEOMETRY (pt_clusters): NVAPI's cluster operations build the
       per-frame model BLASes from 85-triangle CLASes. Only a pipeline made after
       ENABLE_CLUSTER_SUPPORT may trace a clustered BLAS, and the option costs all
       traversal a little - so it is set here, before this renderer makes any
       pipeline that traces, and only with pt_clusters 1, which restarts the renderer
       (CVAR_REFRESH) for a clean A/B. The menu row needs pt_clusters_available. */
    cvar_t *pt_clusters = Cvar_Get("pt_clusters", "0", CVAR_REFRESH);
    const bool clusters_available = d3d12_nvapi_clusters_supported((ID3D12Device *)d3d.device);
    Cvar_FullSet("pt_clusters_available", clusters_available ? "1" : "0", CVAR_ROM, FROM_CODE);
    if (pt_clusters->integer) {
        w.clusters = clusters_available && d3d12_nvapi_cluster_pipelines(d3d.device, true);
        if (w.clusters)
            Com_Printf("D3D12: clustered dynamic BLASes through NVAPI (pt_clusters 1, %d triangles a cluster)\n",
                       PT_CLUSTER_TRIANGLES);
        else
            Com_WPrintf("D3D12: pt_clusters 1, but %s - the dynamic BLASes are built whole\n",
                        clusters_available ? "NVAPI refused the pipeline option" :
                                             "this GPU or driver has no NVAPI cluster operations");
    }

    if (w.ptlas_route) {
        ptlas_t shape;
        d3d12_as_sizes_t pt_trace, pt_build;
        if (ptlas_init(&shape, MAX_TLAS_INSTANCES, PTLAS_MAX_GLOBAL) &&
            d3d12_ptlas_sizes(d3d.device, w.ptlas_route, &shape, true, &pt_trace) &&
            d3d12_ptlas_sizes(d3d.device, w.ptlas_route, &shape, false, &pt_build)) {
            geometry_result = max(geometry_result, max(pt_trace.result, pt_build.result));
            w.geometry_scratch = max(w.geometry_scratch, max(pt_trace.scratch, pt_build.scratch));
            Com_Printf("D3D12: partitioned TLAS available through %s (pt_ptlas 1; %llu KB)\n",
                       d3d12_ptlas_route_name(w.ptlas_route),
                       (unsigned long long)(max(pt_trace.result, pt_build.result) >> 10));
        } else {
            Com_WPrintf("D3D12: %s gave no partitioned TLAS sizes; pt_ptlas is off\n",
                        d3d12_ptlas_route_name(w.ptlas_route));
            w.ptlas_route = D3D12_PTLAS_NONE;
        }
        ptlas_shutdown(&shape);
    }
    const uint64_t upload_size = UPLOAD_PTLAS_OFFSET + (w.ptlas_route ? ptlas_stage_size(MAX_TLAS_INSTANCES) : 0);

    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++) {
        world_frame_t *fr = &w.frame[f];
        fr->upload = mapped_buffer(upload_size, D3D12_HEAP_TYPE_UPLOAD, "world upload", &fr->upload_map);
        if (!fr->upload)
            return false;
        for (int s = 0; s < D3D12_NUM_WORLD_STAGING; s++) {
            const D3D12_HEAP_TYPE heap = s == D3D12_WORLD_STAGING_READBACK ? D3D12_HEAP_TYPE_READBACK
                                                                           : D3D12_HEAP_TYPE_UPLOAD;
            fr->staging[s] = mapped_buffer(staging_sizes[s], heap, "world staging", &fr->staging_map[s]);
            if (!fr->staging[s])
                return false;
        }
        // the geometry TLAS (or PTLAS) and the effects TLAS, each with its own scratch
        if (!d3d12_as_pool_create(&fr->tlas_pool, (ID3D12Device *)d3d.device,
                                  ALIGN_UP(geometry_result, D3D12_AS_ALIGNMENT) +
                                  ALIGN_UP(w.tlas_sizes.result, D3D12_AS_ALIGNMENT) + D3D12_AS_ALIGNMENT, L"TLAS"))
            return false;
        fr->tlas = d3d12_as_pool_alloc(&fr->tlas_pool, geometry_result);
        fr->tlas_effects = d3d12_as_pool_alloc(&fr->tlas_pool, w.tlas_sizes.result);
        fr->tlas_scratch = d3d12_create_buffer(ALIGN_UP(w.geometry_scratch, D3D12_AS_ALIGNMENT) +
                                               ALIGN_UP(w.tlas_sizes.scratch, D3D12_AS_ALIGNMENT),
                                               D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                               "TLAS scratch");
        if (!fr->tlas || !fr->tlas_effects || !fr->tlas_scratch)
            return false;
        if (w.ptlas_route && !ptlas_init(&fr->ptlas, MAX_TLAS_INSTANCES, PTLAS_MAX_GLOBAL))
            return false;
    }
    Cvar_FullSet("pt_ptlas_available", w.ptlas_route ? "1" : "0", CVAR_ROM, FROM_CODE);

    w.light_buffer = d3d12_create_buffer(sizeof(LightBuffer), D3D12_HEAP_TYPE_DEFAULT,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "light buffer");
    w.instance_buffer = d3d12_create_buffer(sizeof(InstanceBuffer), D3D12_HEAP_TYPE_DEFAULT,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "instance buffer");
    w.readback_buffer = d3d12_create_buffer(sizeof(ReadbackBuffer), D3D12_HEAP_TYPE_DEFAULT,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "readback buffer");
    w.tonemap_buffer = d3d12_create_buffer(sizeof(ToneMappingBuffer), D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "tone mapping buffer");
    w.zero_buffer = d3d12_create_buffer(D3D12_ZERO_BUFFER_SIZE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, "zeros");
    // TILE COMPACTION's DISPATCH (one argument, so no root signature) and the copy of a
    // bounce's arguments it reads; without them the bounces cover the whole grid
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg;
        memset(&arg, 0, sizeof(arg));
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sd;
        memset(&sd, 0, sizeof(sd));
        sd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        sd.NumArgumentDescs = 1;
        sd.pArgumentDescs = &arg;
        if (!D3D_CHECK(ID3D12Device5_CreateCommandSignature(d3d.device, &sd, NULL, &IID_ID3D12CommandSignature,
                                                            (void **)&w.tile_dispatch)))
            w.tile_dispatch = NULL;
        w.tile_args = d3d12_create_buffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, "pt tile dispatch");
    }
    w.iqm_buffer = d3d12_create_buffer(sizeof(IqmMatrixBuffer), D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "IQM matrices");
    {
        // vkpt_vertex_buffer_create's radiance cache. Without it the cache stays
        // off (the UBO's pt_sharc zeroed below) rather than the map failing.
        static const char *names[3] = { "SHaRC keys", "SHaRC accumulators", "SHaRC resolved" };
        w.sharc_size[0] = sizeof(uint32_t) * SHARC_CAPACITY;
        w.sharc_size[1] = sizeof(uint32_t) * SHARC_ACCUM_UINTS * SHARC_CAPACITY;
        w.sharc_size[2] = sizeof(float) * 4 * SHARC_RESOLVED_VEC4S * SHARC_CAPACITY;
        for (int i = 0; i < 3; i++)
            w.sharc[i] = d3d12_create_buffer(w.sharc_size[i], D3D12_HEAP_TYPE_DEFAULT,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, names[i]);
        if (!w.sharc[0] || !w.sharc[1] || !w.sharc[2]) {
            Com_WPrintf("D3D12: no memory for the radiance cache - pt_sharc stays off\n");
            for (int i = 0; i < 3; i++)
                D3D12_RELEASE(w.sharc[i]);
        }
        w.sharc_dirty = true;
    }
    if (!w.light_buffer || !w.instance_buffer || !w.readback_buffer || !w.tonemap_buffer || !w.zero_buffer ||
        !w.iqm_buffer)
        return false;
    {
        // committed memory starts zeroed, but say so rather than rely on it
        void *zeros = Z_Mallocz(D3D12_ZERO_BUFFER_SIZE);
        d3d12_upload_buffer(w.zero_buffer, 0, zeros, D3D12_ZERO_BUFFER_SIZE);
        Z_Free(zeros);
    }
    w.tm_reset = true;

    // the path tracer's random numbers; without them every sample is the first
    w.blue_noise = d3d12_blue_noise();
    if (!w.blue_noise)
        Com_WPrintf("D3D12: no blue noise - the lighting will be biased\n");

    // The set instances live as long as the heaps: made once, rewritten in
    // place (with the GPU idle) when what they name changes.
    for (int i = 0; i < 2; i++)
        if (!d3d12_set_alloc(&w.textures[i], D3D12_SET_TEXTURES, i ? "textures odd" : "textures even") ||
            !d3d12_set_alloc(&w.display[i], D3D12_SET_TEXTURES, i ? "display odd" : "display even"))
            return false;
    for (int i = 0; i < 2; i++)
        w.textures[i].external[0] = w.display[i].external[0] = d3d12_bindless_base(i);   // the parity's copy
    if (!d3d12_set_alloc(&w.vertex_buffer_set, D3D12_SET_VERTEX_BUFFER, "vertex buffer") ||
        !d3d12_set_alloc(&w.tiles_set, D3D12_SET_PT_TILES, "pt tiles"))
        return false;
    d3d12_set_write_buffer(&w.vertex_buffer_set, LIGHT_BUFFER_BINDING_IDX, 0, w.light_buffer, 0, sizeof(LightBuffer));
    d3d12_set_write_buffer(&w.vertex_buffer_set, READBACK_BUFFER_BINDING_IDX, 0, w.readback_buffer, 0,
                           sizeof(ReadbackBuffer));
    d3d12_set_write_buffer(&w.vertex_buffer_set, TONE_MAPPING_BUFFER_BINDING_IDX, 0, w.tonemap_buffer, 0,
                           sizeof(ToneMappingBuffer));
    d3d12_set_write_buffer(&w.vertex_buffer_set, IQM_MATRIX_BUFFER_BINDING_IDX, 0, w.iqm_buffer, 0,
                           sizeof(IqmMatrixBuffer));
    if (w.sharc[0]) {
        d3d12_set_write_buffer(&w.vertex_buffer_set, SHARC_KEY_BUFFER_BINDING_IDX, 0, w.sharc[0], 0, w.sharc_size[0]);
        d3d12_set_write_buffer(&w.vertex_buffer_set, SHARC_ACCUM_BUFFER_BINDING_IDX, 0, w.sharc[1], 0,
                               w.sharc_size[1]);
        d3d12_set_write_buffer(&w.vertex_buffer_set, SHARC_RESOLVED_BUFFER_BINDING_IDX, 0, w.sharc[2], 0,
                               w.sharc_size[2]);
    }
    // the instanced buffers come with frontend.c's d3d12_world_set_primbuf_size

    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++) {
        world_frame_t *fr = &w.frame[f];
        if (!d3d12_set_alloc(&fr->ubo_set, D3D12_SET_UBO, "ubo") || !d3d12_set_alloc(&fr->rt_set, D3D12_SET_RT, "rt"))
            return false;
        d3d12_set_write_cbv(&fr->ubo_set, GLOBAL_UBO_BINDING_IDX, 0,
                            ID3D12Resource_GetGPUVirtualAddress(fr->upload) + UPLOAD_UBO_OFFSET, (uint32_t)UPLOAD_UBO_SIZE);
        d3d12_set_write_buffer(&fr->ubo_set, GLOBAL_INSTANCE_BUFFER_BINDING_IDX, 0, w.instance_buffer, 0,
                               sizeof(InstanceBuffer));
        // the transparency texel buffers come with d3d12_world_transparency_init
        d3d12_set_write_as(&fr->rt_set, RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, fr->tlas);
        d3d12_set_write_as(&fr->rt_set, RT_BINDING_TLAS, TLAS_INDEX_EFFECTS, fr->tlas_effects);
    }

    bool psos = true;
    for (int i = 0; i < D3D12_NUM_PASSES; i++)
        psos &= (w.passes[i] = compute_pso(d3d12_pass_info[i].layout, d3d12_pass_info[i].shader)) != NULL;
    // [d3d.hdr]: the frame target's sRGB view, or the scRGB target (vid_hdr)
    w.blit[0] = blit_pso("final_blit.vert", "film_grain.frag", D3D12_FRAME_TARGET_RTV_FORMAT);
    w.blit[1] = blit_pso("final_blit.vert", "film_grain.frag", D3D12_HDR_FORMAT);
    if (!psos || !w.blit[0] || !w.blit[1])
        return false;
    // pt_crt's; without them the blit has grain but no tube
    w.blit_crt[0] = blit_pso("final_blit.vert", "crt.frag", D3D12_FRAME_TARGET_RTV_FORMAT);
    w.blit_crt[1] = blit_pso("final_blit.vert", "crt.frag", D3D12_HDR_FORMAT);

    if (!sky_init()) {
        Com_EPrintf("D3D12: the physical sky failed to start\n");
        return false;
    }

    // without it the 3D view runs with no fog and no god rays
    w.fog_ready = fog_init();
    if (!w.fog_ready) {
        Com_WPrintf("D3D12: the fog did not start (its shadow map is %.0f MB) - no fog or god rays\n",
                    2.0 * SHADOWMAP_SIZE * SHADOWMAP_SIZE * 4 / 1048576.0);
        fog_shutdown();
    }

    w.initialized = true;
    return true;
}

void d3d12_world_shutdown(void)
{
    // the GPU is idle here
    for (int v = 0; v < 2; v++)
        for (int k = 0; k < 2; k++)
            D3D12_RELEASE(w.ser[v][k]);
    D3D12_RELEASE(w.ser_table);
    w.ser_table_map = NULL;
    destroy_images();
    d3d12_world_unload();
    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++) {
        world_frame_t *fr = &w.frame[f];
        D3D12_RELEASE(fr->upload);
        for (int s = 0; s < D3D12_NUM_WORLD_STAGING; s++)
            D3D12_RELEASE(fr->staging[s]);
        d3d12_as_pool_destroy(&fr->tlas_pool);
        D3D12_RELEASE(fr->tlas_scratch);
        ptlas_shutdown(&fr->ptlas);
        D3D12_RELEASE(fr->tr_staging);
        fr->tr_staging_map = NULL;
        for (int i = 0; i < 2; i++) {
            D3D12_RELEASE(fr->blood_staging[i]);
            fr->blood_staging_map[i] = NULL;
        }
        fr->blood_capacity = 0;
        d3d12_dynamic_as_destroy(&fr->dyn);
    }
    for (int i = 0; i <= D3D12_TR_NUM_STREAMS; i++) {
        D3D12_RELEASE(w.tr[i]);
        w.tr_capacity[i] = 0;
    }
    w.tr_frame_capacity = 0;
    for (int i = 0; i < D3D12_MAX_MODELS; i++) {
        D3D12_RELEASE(w.models[i].buffer);
        d3d12_as_pool_destroy(&w.models[i].blas_pool);
    }
    for (int i = 0; i < w.num_model_scratch; i++)
        D3D12_RELEASE(w.model_scratch[i]);
    w.num_model_scratch = 0;
    D3D12_RELEASE(w.iqm_buffer);
    for (int i = 0; i < 3; i++)
        D3D12_RELEASE(w.sharc[i]);
    D3D12_RELEASE(w.prims_instanced);
    D3D12_RELEASE(w.positions_instanced);
    w.primbuf_size = 0;
    D3D12_RELEASE(w.light_buffer);
    D3D12_RELEASE(w.instance_buffer);
    D3D12_RELEASE(w.readback_buffer);
    D3D12_RELEASE(w.tonemap_buffer);
    D3D12_RELEASE(w.zero_buffer);
    destroy_tiles();
    w.tiles_failed_size = 0;
    D3D12_RELEASE(w.tile_args);
    D3D12_RELEASE(w.tile_dispatch);
    w.blue_noise = NULL;    // textures.c's
    for (int i = 0; i < D3D12_NUM_PASSES; i++)
        D3D12_RELEASE(w.passes[i]);
    D3D12_RELEASE(w.blit[0]);
    D3D12_RELEASE(w.blit[1]);
    D3D12_RELEASE(w.blit_crt[0]);
    D3D12_RELEASE(w.blit_crt[1]);
    fog_shutdown();
    sky_shutdown();
    w.initialized = false;
}

bool d3d12_world_ready(void)
{
    return w.initialized;
}

void *d3d12_world_staging(int which)
{
    if (!w.initialized || which < 0 || which >= D3D12_NUM_WORLD_STAGING)
        return NULL;
    return w.frame[d3d.frame_index].staging_map[which];
}

/* ------------------------------------------------------------------------- */
/* THE MAP                                                                    */
/* ------------------------------------------------------------------------- */

/*
Static BLASes over runs of `buffer`'s positions - the world's five classes and
brush models at map load, a static model's three kinds (vkpt's build_model_blas):
each d3d12_world_blas_t's geometries are prim_offsets / prim_counts in
triangles of 9 floats from positions_offset. Recorded into the upload list,
right after the buffer's own upload (so in COPY_DEST), into a new pool of their
own; every build has its own scratch region, so they need no barriers between
them. The scratch is the caller's, to release once the GPU is past it. Fills
in each blases[i].address (0: none built). False: out of memory.
*/
static bool build_static_blases(ID3D12Resource *buffer, uint64_t positions_offset, d3d12_world_blas_t *blases,
                                int num_blases, d3d12_as_pool_t *pool, const wchar_t *pool_name,
                                ID3D12Resource **scratch_out)
{
    *scratch_out = NULL;
    for (int i = 0; i < num_blases; i++)
        blases[i].address = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS positions = ID3D12Resource_GetGPUVirtualAddress(buffer) + positions_offset;

    // every BLAS's geometry list and sizes
    int max_geometries = 0;
    for (int i = 0; i < num_blases; i++)
        max_geometries = max(max_geometries, (int)blases[i].num_geometries);
    d3d12_as_triangles_t *geoms = Z_Mallocz(sizeof(*geoms) * (size_t)max(max_geometries, 1) * max(num_blases, 1));
    d3d12_as_sizes_t *sizes = Z_Mallocz(sizeof(*sizes) * (size_t)max(num_blases, 1));
    int *counts = Z_Mallocz(sizeof(*counts) * (size_t)max(num_blases, 1));
    uint64_t pool_size = 0, scratch_size = 0;
    for (int i = 0; i < num_blases; i++) {
        const d3d12_world_blas_t *b = &blases[i];
        d3d12_as_triangles_t *g = geoms + (size_t)i * max_geometries;
        int n = 0;
        for (uint32_t k = 0; k < b->num_geometries; k++) {
            if (!b->prim_counts[k])
                continue;
            g[n].vertices = positions + (uint64_t)b->prim_offsets[k] * 9 * sizeof(float);
            g[n].num_vertices = b->prim_counts[k] * 3;
            n++;
        }
        counts[i] = n;
        if (!n || !d3d12_blas_sizes(d3d.device, g, n, BLAS_BUILD_FLAGS, &sizes[i])) {
            counts[i] = 0;
            continue;
        }
        pool_size += ALIGN_UP(sizes[i].result, D3D12_AS_ALIGNMENT);
        scratch_size += ALIGN_UP(sizes[i].scratch, D3D12_AS_ALIGNMENT);
    }

    bool ok = true;
    ID3D12GraphicsCommandList4 *cmd = NULL;
    if (pool_size) {
        ID3D12GraphicsCommandList *upload = d3d12_upload_list();
        ok = upload &&
             SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(upload, &IID_ID3D12GraphicsCommandList4, (void **)&cmd)) &&
             d3d12_as_pool_create(pool, (ID3D12Device *)d3d.device, pool_size, pool_name);
        if (ok) {
            *scratch_out = d3d12_create_buffer(scratch_size, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "static BLAS scratch");
            ok = *scratch_out != NULL;
        }
    }

    if (ok && pool_size) {
        // the copy promoted the buffer to COPY_DEST; a build reads its
        // geometry as a non-pixel shader resource
        transition(cmd, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_GPU_VIRTUAL_ADDRESS s = ID3D12Resource_GetGPUVirtualAddress(*scratch_out);
        for (int i = 0; i < num_blases; i++) {
            if (!counts[i])
                continue;
            const D3D12_GPU_VIRTUAL_ADDRESS dest = d3d12_as_pool_alloc(pool, sizes[i].result);
            if (!dest)
                break;
            d3d12_blas_build(cmd, geoms + (size_t)i * max_geometries, counts[i], BLAS_BUILD_FLAGS, dest, s, 0);
            s += ALIGN_UP(sizes[i].scratch, D3D12_AS_ALIGNMENT);
            blases[i].address = dest;
        }
        d3d12_as_barrier(cmd, pool);
    }
    if (cmd)
        ID3D12GraphicsCommandList4_Release(cmd);
    Z_Free(geoms);
    Z_Free(sizes);
    Z_Free(counts);

    if (!ok)
        for (int i = 0; i < num_blases; i++)
            blases[i].address = 0;
    return ok;
}

void d3d12_world_unload(void)
{
    if (!w.world_buffer && !w.blas_pool.buffer && !w.light_stats[0])
        return;
    d3d12_wait_idle();
    D3D12_RELEASE(w.world_buffer);
    // no descriptor left on the freed buffer: the model view draws with no map
    d3d12_set_write_buffer(&w.vertex_buffer_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_WORLD, NULL, 0, 0);
    d3d12_as_pool_destroy(&w.blas_pool);
    for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++) {
        if (!w.light_stats[i])
            continue;
        D3D12_RELEASE(w.light_stats[i]);
        d3d12_set_write_buffer(&w.vertex_buffer_set, LIGHT_STATS_BUFFER_BINDING_IDX, i, NULL, 0, 0);
    }
}

bool d3d12_world_loaded(void)
{
    return w.world_buffer != NULL;
}

bool d3d12_world_load(d3d12_world_map_t *map)
{
    d3d12_world_unload();
    for (int i = 0; i < map->num_blases; i++)
        map->blases[i].address = 0;
    if (!w.initialized || !map->size)
        return false;

    w.world_buffer = d3d12_create_buffer(map->size, D3D12_HEAP_TYPE_DEFAULT,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "world");
    if (!w.world_buffer)
        return false;
    d3d12_upload_buffer(w.world_buffer, 0, map->data, map->size);
    ID3D12Resource *scratch = NULL;
    const bool ok = build_static_blases(w.world_buffer, map->positions_offset, map->blases, map->num_blases,
                                        &w.blas_pool, L"world BLASes", &scratch);
    // a partitioned TLAS keeps its instances' bounds: new BLASes, maybe at old addresses
    ptlas_static_changed();
    d3d12_upload_flush();
    d3d12_wait_idle();
    D3D12_RELEASE(scratch);

    if (!ok) {
        Com_EPrintf("D3D12: the world's acceleration structures failed\n");
        for (int i = 0; i < map->num_blases; i++)
            map->blases[i].address = 0;
        return false;
    }

    // nothing is in flight: the descriptors can change under no frame
    d3d12_set_write_buffer(&w.vertex_buffer_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_WORLD, w.world_buffer, 0,
                           map->primitives_size);
    // the shadow map draws the world from here
    w.world_positions_offset = map->positions_offset;

    // vkpt_light_buffers_create's; the first frame clears all three. Without
    // them the light sampling runs without its shadow statistics (a null
    // buffer reads 0: no correction), so a failure is not the map's.
    w.light_stats_size = max(map->light_stats_count, 1) * sizeof(uint32_t);
    for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++) {
        w.light_stats[i] = d3d12_create_buffer(w.light_stats_size, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "light stats");
        if (!w.light_stats[i]) {
            Com_WPrintf("D3D12: no light statistics buffers (%.1f MB each)\n", w.light_stats_size / 1048576.0);
            for (int k = 0; k < i; k++)
                D3D12_RELEASE(w.light_stats[k]);
            break;
        }
    }
    for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
        d3d12_set_write_buffer(&w.vertex_buffer_set, LIGHT_STATS_BUFFER_BINDING_IDX, i, w.light_stats[i], 0,
                               w.light_stats[i] ? w.light_stats_size : 0);
    w.light_stats_fresh = true;
    // a new map: the eye adapts afresh (vkpt_begin_registration's request)
    w.tm_reset = true;
    Com_DPrintf("D3D12: world %.1f MB, %d BLASes in %.1f MB\n", map->size / 1048576.0, map->num_blases,
                w.blas_pool.size / 1048576.0);
    return true;
}

/* ------------------------------------------------------------------------- */
/* THE MODELS                                                                 */
/* ------------------------------------------------------------------------- */

void d3d12_world_model_free(int index)
{
    if (index < 0 || index >= D3D12_MAX_MODELS)
        return;
    world_model_t *m = &w.models[index];
    if (!m->buffer && !m->blas_pool.buffer)
        return;
    // a frame in flight may still read them: gone once it is done (and the
    // commit waits for the GPU anyway)
    D3D12_DEFER_RELEASE(m->buffer);
    D3D12_DEFER_RELEASE(m->blas_pool.buffer);
    memset(&m->blas_pool, 0, sizeof(m->blas_pool));
    m->primitives_size = 0;
    m->dirty = true;
}

bool d3d12_world_model_upload(int index, d3d12_world_model_t *model)
{
    for (int i = 0; i < model->num_blases; i++)
        model->blases[i].address = 0;
    if (!w.initialized || index < 0 || index >= D3D12_MAX_MODELS || !model->size)
        return false;
    d3d12_world_model_free(index);
    world_model_t *m = &w.models[index];
    m->dirty = true;
    // as the world's: a partitioned TLAS starts again from nothing
    ptlas_static_changed();

    // vkpt_vertex_buffer_upload_models' buffer, without the BLAS memory: the
    // shaders read and write it as a raw UAV, like every buffer here
    m->buffer = d3d12_create_buffer(model->size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    "model");
    if (!m->buffer)
        return false;
    d3d12_upload_buffer(m->buffer, 0, model->data, model->size);
    m->primitives_size = model->primitives_size;
    if (!model->num_blases)
        return true;

    ID3D12Resource *scratch = NULL;
    const bool ok = build_static_blases(m->buffer, model->positions_offset, model->blases, model->num_blases,
                                        &m->blas_pool, L"model BLASes", &scratch);
    if (scratch) {
        if (w.num_model_scratch < D3D12_MAX_MODELS)
            w.model_scratch[w.num_model_scratch++] = scratch;
        else
            D3D12_DEFER_RELEASE(scratch);   // (never: one per model at most)
    }
    if (!ok) {
        // the VBO alone: drawn by nothing, as a static model without its BLAS
        Com_WPrintf("D3D12: model %d's acceleration structures failed\n", index);
        for (int i = 0; i < model->num_blases; i++)
            model->blases[i].address = 0;
    }
    return true;
}

void d3d12_world_models_commit(void)
{
    // the copies and builds are done, and no frame reads the descriptors the
    // loop below rewrites
    d3d12_upload_flush();
    d3d12_wait_idle();
    for (int i = 0; i < w.num_model_scratch; i++)
        D3D12_RELEASE(w.model_scratch[i]);
    w.num_model_scratch = 0;

    for (int i = 0; i < D3D12_MAX_MODELS; i++) {
        world_model_t *m = &w.models[i];
        if (!m->dirty)
            continue;
        m->dirty = false;
        d3d12_set_write_buffer(&w.vertex_buffer_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_FIRST_MODEL + i,
                               m->buffer, 0, m->buffer ? m->primitives_size : 0);
    }
}

uint32_t d3d12_world_primbuf_size(void)
{
    return w.primbuf_size;
}

void d3d12_world_set_primbuf_size(uint32_t prims)
{
    if (!w.initialized || prims == w.primbuf_size)
        return;
    // vkpt_vertex_buffer_ensure_primbuf_size: made afresh, nothing kept
    d3d12_wait_idle();
    D3D12_RELEASE(w.prims_instanced);
    D3D12_RELEASE(w.positions_instanced);
    w.primbuf_size = 0;

    w.prims_instanced = d3d12_create_buffer((uint64_t)prims * sizeof(VboPrimitive), D3D12_HEAP_TYPE_DEFAULT,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "instanced primitives");
    w.positions_instanced = d3d12_create_buffer((uint64_t)prims * 9 * sizeof(float), D3D12_HEAP_TYPE_DEFAULT,
                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "instanced positions");
    if (w.prims_instanced && w.positions_instanced) {
        w.primbuf_size = prims;
    } else {
        Com_EPrintf("D3D12: no animation buffers for %u primitives (%.1f MB) - no models will be drawn\n", prims,
                    prims * (sizeof(VboPrimitive) + 9 * sizeof(float)) / 1048576.0);
        D3D12_RELEASE(w.prims_instanced);
        D3D12_RELEASE(w.positions_instanced);
    }
    d3d12_set_write_buffer(&w.vertex_buffer_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_INSTANCED,
                           w.prims_instanced, 0, (uint64_t)w.primbuf_size * sizeof(VboPrimitive));
    d3d12_set_write_buffer(&w.vertex_buffer_set, POSITION_BUFFER_BINDING_IDX, 0, w.positions_instanced, 0,
                           (uint64_t)w.primbuf_size * 9 * sizeof(float));
}

bool d3d12_world_transparency_init(const uint64_t capacity[D3D12_TR_NUM_STREAMS + 1], uint64_t frame_capacity,
                                   const uint16_t *indices)
{
    if (!w.initialized)
        return false;
    // initialize_transparency's buffers, and the host buffer's slices (one a slot)
    static const char *names[D3D12_TR_NUM_STREAMS + 1] = {
        "transparency vertices", "beam AABBs", "particle colours", "beam colours", "sprite infos",
        "beam intersections", "transparency indices",
    };
    bool ok = true;
    for (int i = 0; i <= D3D12_TR_NUM_STREAMS; i++) {
        D3D12_RELEASE(w.tr[i]);
        w.tr_capacity[i] = capacity[i];
        w.tr[i] = d3d12_create_buffer(max(capacity[i], (uint64_t)16), D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_NONE, names[i]);
        ok &= w.tr[i] != NULL;
    }
    w.tr_frame_capacity = frame_capacity;
    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT && ok; f++) {
        world_frame_t *fr = &w.frame[f];
        D3D12_RELEASE(fr->tr_staging);
        fr->tr_staging = mapped_buffer(max(frame_capacity, (uint64_t)16), D3D12_HEAP_TYPE_UPLOAD,
                                       "transparency staging", &fr->tr_staging_map);
        ok &= fr->tr_staging != NULL;
    }
    if (!ok) {
        Com_EPrintf("D3D12: no transparency buffers - particles, beams and sprites are not drawn\n");
        for (int i = 0; i <= D3D12_TR_NUM_STREAMS; i++)
            D3D12_RELEASE(w.tr[i]);
        for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++) {
            D3D12_RELEASE(w.frame[f].tr_staging);
            w.frame[f].tr_staging_map = NULL;
        }
        return false;
    }

    // fill_index_buffer's quads, once
    d3d12_upload_buffer(w.tr[D3D12_TR_INDICES], 0, indices, capacity[D3D12_TR_INDICES]);
    d3d12_upload_flush();
    d3d12_wait_idle();

    // create_buffer_views': what pt_logic_particle / _beam / _sprite fetch
    static const struct { int stream, binding; DXGI_FORMAT format; } views[] = {
        { D3D12_TR_PARTICLE_COLORS, RT_BINDING_PARTICLE_COLOR,  DXGI_FORMAT_R32G32B32A32_FLOAT },
        { D3D12_TR_BEAM_COLORS,     RT_BINDING_BEAM_COLOR,      DXGI_FORMAT_R32G32B32A32_FLOAT },
        { D3D12_TR_SPRITE_INFOS,    RT_BINDING_SPRITE_INFO,     DXGI_FORMAT_R32G32B32A32_UINT },
        { D3D12_TR_BEAM_INTERSECT,  RT_BINDING_BEAM_INTERSECT,  DXGI_FORMAT_R32G32B32A32_UINT },
    };
    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++)
        for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++)
            d3d12_set_write_texel_buffer(&w.frame[f].rt_set, views[i].binding, 0, w.tr[views[i].stream],
                                         views[i].format, 0, (uint32_t)(capacity[views[i].stream] / 16));
    return true;
}

void d3d12_world_plan_dynamic(const uint32_t first[D3D12_NUM_DYN_RUNS], const uint32_t count[D3D12_NUM_DYN_RUNS],
                              const d3d12_world_transparency_t *tr, bool fast_trace, bool blood_reuse,
                              uint64_t address[D3D12_NUM_DYN], bool *blood_built)
{
    world_frame_t *fr = &w.frame[d3d.frame_index];
    *blood_built = false;
    fr->dyn_flags = fast_trace ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                               : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    memset(fr->dyn_blases, 0, sizeof(fr->dyn_blases));

    // the runs of the instanced positions - nothing past their end, where
    // instance_geometry wrote nothing
    if (w.positions_instanced) {
        const D3D12_GPU_VIRTUAL_ADDRESS positions = ID3D12Resource_GetGPUVirtualAddress(w.positions_instanced);
        for (int i = 0; i < D3D12_NUM_DYN_RUNS; i++)
            if (first[i] < w.primbuf_size)
                fr->dyn_blases[i].geometry = d3d12_triangle_run(positions, first[i],
                                                                min(count[i], w.primbuf_size - first[i]));
    }

    // vkpt_get_transparency_buffers / vkpt_get_beam_aabb_buffer: the particles'
    // quads from vertex 0, the sprites' after them, both through the one index
    // list; the beams' boxes. Bounded by the buffers, as transparency.c's counts are.
    // Only what d3d12_world_render will copy: the frame fits the staging, and
    // the particles' and sprites' quads fit the vertex buffer together.
    if (w.tr[0] && fr->tr_staging_map && tr && tr->data && tr->upload_size &&
        tr->upload_size <= w.tr_frame_capacity) {
        const D3D12_GPU_VIRTUAL_ADDRESS vertices = ID3D12Resource_GetGPUVirtualAddress(w.tr[D3D12_TR_VERTICES]);
        const D3D12_GPU_VIRTUAL_ADDRESS indices = ID3D12Resource_GetGPUVirtualAddress(w.tr[D3D12_TR_INDICES]);
        const uint64_t max_quads = w.tr_capacity[D3D12_TR_VERTICES] / (4 * 3 * sizeof(float));
        const bool quads_fit = (uint64_t)tr->particle_num + tr->sprite_num <= max_quads;
        if (tr->particle_num && quads_fit) {
            d3d12_as_triangles_t *g = &fr->dyn_blases[D3D12_DYN_PARTICLES].geometry;
            g->vertices = vertices;
            g->num_vertices = tr->particle_num * 4;
            g->indices = indices;
            g->num_indices = tr->particle_num * 6;
        }
        if (tr->sprite_num && quads_fit) {
            d3d12_as_triangles_t *g = &fr->dyn_blases[D3D12_DYN_SPRITES].geometry;
            g->vertices = vertices + tr->sprite_vertex_offset;
            g->num_vertices = tr->sprite_num * 4;
            g->indices = indices;
            g->num_indices = tr->sprite_num * 6;
        }
        if (tr->beam_num && tr->beam_num <= w.tr_capacity[D3D12_TR_BEAM_AABBS] / sizeof(D3D12_RAYTRACING_AABB)) {
            d3d12_as_triangles_t *g = &fr->dyn_blases[D3D12_DYN_BEAMS].geometry;
            g->aabbs = ID3D12Resource_GetGPUVirtualAddress(w.tr[D3D12_TR_BEAM_AABBS]);
            g->num_aabbs = tr->beam_num;
        }
    }

    // pt_clusters: the model runs of the geometry TLAS. Not the explosions, whose hits
    // (pt_logic_explosion) read no geometry index - and NOT THE BLOOD: blood.c pads
    // every droplet slot to its stride with zeroed triangles, so a cluster of 85 in a
    // row holding a droplet's last faces and the padding after them spans the droplet
    // AND the world origin. A floor of splats made thousands of such boxes, all
    // crossing at the origin, and tracing crawled (Matt, 11-06). A whole build sorts
    // the zeros into their own corner, so blood stays whole.
    const bool clustered = w.clusters && !w.clusters_failed;
    if (clustered)
        for (int i = D3D12_DYN_OPAQUE; i <= D3D12_DYN_VIEWER_MODELS; i++)
            fr->dyn_blases[i].clustered = true;

    // pt_blood_blas_reuse, as vkpt_pt_create_all_dynamic does it: settled blood's BLAS
    // stands in the slot's persistent memory, unbuilt, while blood.c says the section
    // is unchanged since this slot built it
    fr->dyn_blases[D3D12_DYN_BLOOD].persistent = true;
    fr->dyn_blases[D3D12_DYN_BLOOD].reuse = blood_reuse;

    // R_BeginFrame waited for this slot's last frame: its memory is free
    if (!w.initialized ||
        !d3d12_dynamic_blas_plan(d3d.device, &fr->dyn, fr->dyn_blases, D3D12_NUM_DYN, fr->dyn_flags)) {
        static bool warned;
        if (w.initialized && !warned) {
            warned = true;
            Com_EPrintf("D3D12: no memory for the dynamic BLASes - models and effects are not drawn\n");
        }
        for (int i = 0; i < D3D12_NUM_DYN; i++)
            fr->dyn_blases[i].address = 0;
    }
    for (int i = 0; i < D3D12_NUM_DYN; i++)
        address[i] = fr->dyn_blases[i].address;
    *blood_built = fr->dyn_blases[D3D12_DYN_BLOOD].address && !fr->dyn_blases[D3D12_DYN_BLOOD].reused;

    // the plan clears the flag where NVAPI would not size the clusters
    if (clustered) {
        static bool warned;
        for (int i = D3D12_DYN_OPAQUE; i <= D3D12_DYN_VIEWER_MODELS && !warned; i++)
            if (fr->dyn_blases[i].address && !fr->dyn_blases[i].clustered) {
                warned = true;
                Com_WPrintf("D3D12: NVAPI gave no sizes for the clustered BLASes (pt_clusters) - built whole\n");
            }
    }
}

/* ------------------------------------------------------------------------- */
/* A FRAME                                                                    */
/* ------------------------------------------------------------------------- */

static void update_display_image(void)
{
    if (w.show->modified) {
        w.show->modified = false;
        int image = find_image(w.show->string);
        if (image < 0)
            Com_WPrintf("d3d12_show: no G-buffer image \"%s\" - names as in global_textures.h, e.g. PT_ALBEDO\n",
                        w.show->string);
        else
            w.show_image = image;
    }

    /* pt_dlss_debug - the DLSS Debug menu row - shows the same views as vkpt, from the
       one table in DLSS.c, raw like vkpt's blit. It used to only swap DLSS's colour
       input here, so the screen showed RR's reconstruction of a normal map, and only
       for 1..22. An A/B pair needs nothing extra: the display sets put the image's
       BINDING at TAA_OUTPUT's, and each parity's set already holds that frame's image. */
    int image = DLSSDebugImage(NULL);
    if (image < 0)
        image = w.show_image;
    if (image == w.display_image)
        return;
    // the display sets are rewritten in place
    d3d12_wait_idle();
    w.display_image = image;
    if (w.images_made)
        write_display_sets();
}

#define BLOOD_POSITION_SIZE (9 * sizeof(float))    // prim_positions_t

/* One frame slot's blood staging at `capacity` primitives (blood.c's shadow size),
   the old one released - the caller knows the slot's last frame is done. Not
   zeroed: upload_blood copies out only the regions it has just written. */
static bool make_blood_staging(world_frame_t *fr, uint32_t capacity)
{
    for (int i = 0; i < 2; i++) {
        D3D12_RELEASE(fr->blood_staging[i]);
        fr->blood_staging_map[i] = NULL;
    }
    fr->blood_capacity = 0;
    fr->blood_staging[0] = mapped_buffer_ex((uint64_t)capacity * sizeof(VboPrimitive), D3D12_HEAP_TYPE_UPLOAD,
                                            "blood staging", &fr->blood_staging_map[0], false);
    fr->blood_staging[1] = mapped_buffer_ex((uint64_t)capacity * BLOOD_POSITION_SIZE, D3D12_HEAP_TYPE_UPLOAD,
                                            "blood position staging", &fr->blood_staging_map[1], false);
    if (fr->blood_staging[0] && fr->blood_staging[1]) {
        fr->blood_capacity = capacity;
        return true;
    }
    for (int i = 0; i < 2; i++) {
        D3D12_RELEASE(fr->blood_staging[i]);
        fr->blood_staging_map[i] = NULL;
    }
    static bool warned;
    if (!warned) {
        warned = true;
        Com_EPrintf("D3D12: no memory for the blood staging - droplets are not updated\n");
    }
    return false;
}

void d3d12_world_reserve_blood_staging(uint32_t capacity)
{
    if (!w.initialized || !capacity)
        return;
    // an exact size, as Vulkan's vk_staging_ensure: a lowered cl_blood_max gives
    // the memory back at the next map
    bool idle = false;
    for (int f = 0; f < D3D12_FRAMES_IN_FLIGHT; f++) {
        world_frame_t *fr = &w.frame[f];
        if (fr->blood_capacity == capacity)
            continue;
        if (!idle) {
            d3d12_wait_idle();      // the slot's last frame may still read it
            idle = true;
        }
        make_blood_staging(fr, capacity);
    }
}

/* vkpt_blood_update's device half: blood.c's changed regions through this
   slot's staging into the instanced buffers (d3d12_record_blood_upload, which
   also zeroes the section on an epoch frame). True when it wrote: both buffers
   are then COPY_DEST. */
static bool upload_blood(ID3D12GraphicsCommandList4 *cmd, world_frame_t *fr, const d3d12_world_blood_t *b)
{
    if (!b->prim_count || !w.prims_instanced || !w.positions_instanced || b->prim_offset >= w.primbuf_size)
        return false;

    // the shadow copy's size, as blood.c's Vulkan staging; this slot's last
    // frame is done (R_BeginFrame waited), so its staging may be remade here.
    // Normally already made at map load (d3d12_world_reserve_blood_staging); this
    // is for blood turned on, or made bigger, mid-map.
    if (b->num_regions && fr->blood_capacity < b->capacity)
        make_blood_staging(fr, b->capacity);

    // the regions' bytes into the staging, at their own offsets (what the copies read);
    // timed for pt_blood_stats (write-combined memory: the copy is the cost)
    const uint64_t copy_t0 = Sys_Microseconds();
    const uint32_t have = min(fr->blood_capacity, b->capacity);
    if (b->num_regions && !have)
        w.blood_lost = true;
    for (uint32_t r = 0; r < b->num_regions; r++) {
        const d3d12_blood_region_t *g = &b->regions[r];
        if (g->src > have || g->count > have - g->src)
            continue;
        memcpy(fr->blood_staging_map[0] + (size_t)g->src * sizeof(VboPrimitive),
               (const uint8_t *)b->prims + (size_t)g->src * sizeof(VboPrimitive), (size_t)g->count * sizeof(VboPrimitive));
        memcpy(fr->blood_staging_map[1] + (size_t)g->src * BLOOD_POSITION_SIZE,
               (const uint8_t *)b->positions + (size_t)g->src * BLOOD_POSITION_SIZE,
               (size_t)g->count * BLOOD_POSITION_SIZE);
    }
    w.blood_copy_usec = Sys_Microseconds() - copy_t0;

    const d3d12_blood_upload_t up = {
        .prims = w.prims_instanced,
        .positions = w.positions_instanced,
        .staging_prims = have ? fr->blood_staging[0] : NULL,
        .staging_positions = have ? fr->blood_staging[1] : NULL,
        .staging_capacity = have,
        .prim_size = sizeof(VboPrimitive),
        .position_size = BLOOD_POSITION_SIZE,
        .zeros = w.zero_buffer,
        .first = b->prim_offset,
        // frontend.c grows the instanced buffers to the reservation first; clipped anyway
        .end = (uint32_t)min((uint64_t)b->prim_offset + b->prim_count, (uint64_t)w.primbuf_size),
        .zero_section = b->zero_section,
        .regions = b->regions,
        .num_regions = b->num_regions,
    };
    return d3d12_record_blood_upload(cmd, &up);
}

uint64_t d3d12_world_blood_copy_usec(void)
{
    return w.blood_copy_usec;
}

bool d3d12_world_take_blood_lost(void)
{
    const bool lost = w.blood_lost;
    w.blood_lost = false;
    return lost;
}

bool d3d12_world_render(ID3D12GraphicsCommandList4 *cmd, const d3d12_world_frame_t *frame)
{
    w.last_drawn = false;
    // the player setup's model view draws with no map loaded (nothing below reads
    // the world buffer without it: no world instances, fog, sky or animation)
    if (!w.initialized || (!w.world_buffer && !frame->no_world))
        return false;

    update_display_image();
    if (!ensure_images(&frame->extents))
        return false;
    // TILE COMPACTION: the lists this view's header names, if it has them
    const bool tiles = frame->tile_header && ensure_tiles(frame->tile_header);

    const int f = d3d.frame_index;
    const int odd = (int)(d3d.frame_counter & 1);
    world_frame_t *fr = &w.frame[f];
    const QVKUniformBuffer_t *ubo = frame->ubo;
    // the fog, and the sun's shadow map it reads
    const bool fog = frame->god_rays && !frame->no_world && w.world_buffer && d3d12_world_fog_ready() &&
                     w.sky_shadowmap_pso;

    // what the shaders read this frame
    const uint32_t num_instances = min(frame->num_instances, (uint32_t)MAX_TLAS_INSTANCES);
    QVKUniformBuffer_t *gpu_ubo = (QVKUniformBuffer_t *)(fr->upload_map + UPLOAD_UBO_OFFSET);
    memcpy(gpu_ubo, ubo, sizeof(QVKUniformBuffer_t));
    // the radiance cache: vkpt_prepare_frame zeroed pt_sharc when its update
    // does not run; here also when its buffers could not be made
    const bool sharc = frame->sharc_active && w.sharc[0];
    if (!sharc)
        gpu_ubo->pt_sharc = 0.f;
    memcpy(fr->upload_map + UPLOAD_INSTANCE_OFFSET, frame->instance_buffer, sizeof(InstanceBuffer));
    memcpy(fr->upload_map + UPLOAD_TLAS_OFFSET, frame->instances,
           (size_t)num_instances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

    // the profiler's rows (gpu_timers.c), marked where vkpt marks them; FRAME_TIME
    // runs from here to DLSS (passes.c), UPLOAD_LIGHTS is vkpt's transfer (the
    // lights and the bones; the instance buffer's copy rides along)
    d3d12_timers_t timers;
    d3d12_timers_get(&timers);
    d3d12_timer_mark(cmd, &timers, PROFILER_FRAME_TIME, PROFILER_START);
    d3d12_timer_mark(cmd, &timers, PROFILER_UPLOAD_LIGHTS, PROFILER_START);

    bool lights_copied = false;
    for (uint32_t i = 0; i < frame->num_light_regions && i < 2; i++)
        if (frame->light_sizes[i]) {
            ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, w.light_buffer, frame->light_offsets[i],
                                                        fr->staging[D3D12_WORLD_STAGING_LIGHTS],
                                                        frame->light_offsets[i], frame->light_sizes[i]);
            lights_copied = true;
        }
    ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, w.instance_buffer, 0, fr->upload, UPLOAD_INSTANCE_OFFSET,
                                                sizeof(InstanceBuffer));
    // the skeletal models' bones, as far as vkpt says they were ever written
    const uint64_t iqm_size = min(frame->iqm_upload_size, (uint64_t)sizeof(IqmMatrixBuffer));
    if (iqm_size)
        ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, w.iqm_buffer, 0, fr->staging[D3D12_WORLD_STAGING_IQM], 0,
                                                    iqm_size);
    d3d12_timer_mark(cmd, &timers, PROFILER_UPLOAD_LIGHTS, PROFILER_STOP);
    // promoted to COPY_DEST by the copies, which a buffer cannot leave by
    // promotion; they decay back to COMMON once the list is done. A buffer
    // nothing copied into stays COMMON and is promoted by the shaders' use.
    if (lights_copied)
        transition(cmd, w.light_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd, w.instance_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (iqm_size)
        transition(cmd, w.iqm_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // upload_geometry: the frame's particles, beams and sprites, each stream to
    // the start of its buffer, then readable by the BLAS builds and the hit
    // shaders' texel fetches. A stream with nothing this frame is left as it is.
    const d3d12_world_transparency_t *tr = &frame->tr;
    bool tr_quads = false;
    if (w.tr[0] && fr->tr_staging_map && tr->data && tr->upload_size && tr->upload_size <= w.tr_frame_capacity) {
        memcpy(fr->tr_staging_map, tr->data, tr->upload_size);
        for (int i = 0; i < D3D12_TR_NUM_STREAMS; i++) {
            if (!tr->size[i] || tr->size[i] > w.tr_capacity[i] || tr->offset[i] + tr->size[i] > tr->upload_size)
                continue;
            ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, w.tr[i], 0, fr->tr_staging, tr->offset[i], tr->size[i]);
            transition(cmd, w.tr[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        tr_quads = tr->size[D3D12_TR_VERTICES] != 0;
    }
    // the index list decayed to COMMON since its upload; a build input
    if (tr_quads)
        transition(cmd, w.tr[D3D12_TR_INDICES], D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // this frame's light statistics start from zero (vkpt_light_buffer_upload_to_staging's fill)
    if (w.light_stats[0]) {
        for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
            if (w.light_stats_fresh || i == (int)(ubo->current_frame_idx % NUM_LIGHT_STATS_BUFFERS))
                d3d12_clear_buffer(cmd, w.light_stats[i], w.light_stats_size, w.zero_buffer);
        w.light_stats_fresh = false;
    }

    ID3D12DescriptorHeap *heaps[2] = { d3d.srv_heap, d3d.sampler_heap };
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cmd, 2, heaps);

    // TILE COMPACTION: this view's header, in the form passes.c dispatches
    if (tiles) {
        uint32_t header[PT_TILE_HDR_WORDS];
        memcpy(header, frame->tile_header, sizeof(header));
        d3d12_tile_header(header, D3D12_TILE_ROW_GROUPS);
        memcpy(fr->tile_reset_map, header, sizeof(header));
    }

    // the passes, as tests/d3d12_world_test.c records them too
    d3d12_passes_t p = {
        .cmd = cmd,
        .enhanced_barriers = d3d.caps.enhanced_barriers,
        .psos = w.passes,
        .pt_sets = { &fr->rt_set, &fr->ubo_set, &w.textures[odd], &w.vertex_buffer_set, &w.tiles_set },
        .standard_sets = { &fr->ubo_set, &w.textures[odd], &w.vertex_buffer_set },
        .god_rays_sets = { fog ? &fr->god_rays_set : NULL, &w.vertex_buffer_set, &fr->ubo_set, &w.textures[odd] },
        .tonemap_buffer = w.tonemap_buffer,
        .tonemap_size = sizeof(ToneMappingBuffer),
        .zero_buffer = w.zero_buffer,
        .sharc_buffers = { w.sharc[0], w.sharc[1], w.sharc[2] },
        .sharc_sizes = { w.sharc_size[0], w.sharc_size[1], w.sharc_size[2] },
        .sharc_entries = SHARC_CAPACITY,
        .upscale = frame->dlss && !frame->dlss_bypass ? d3d12_frontend_dlss_record : NULL,
        .neural = frame->dlss5 ? d3d12_frontend_dlss5_record : NULL,
        .heaps = heaps,
        .num_heaps = 2,
        .tile_buffer = tiles ? w.tiles : NULL,
        .tile_reset = fr->tile_reset,
        .tile_reset_size = w.tiles_reset_size,
        .tile_args = w.tile_args,
        .tile_dispatch = w.tile_dispatch,
    };
    ser_select(&p);
    p.timers = timers;

    // the sun's shadow map, its static half (shadow_map_static says why here)
    if (fog) {
        d3d12_timer_mark(cmd, &timers, PROFILER_SHADOW_MAP, PROFILER_START);
        shadow_map_static(cmd, frame);
        d3d12_timer_mark(cmd, &timers, PROFILER_SHADOW_MAP, PROFILER_STOP);
    }

    // vkpt_blood_update: the droplets' section of the instanced buffers, ahead
    // of instance_geometry (as vkpt records it), which writes other sections
    const bool blood_wrote = upload_blood(cmd, fr, &frame->blood);
    if (blood_wrote)
        transition(cmd, w.prims_instanced, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // vkpt_instance_geometry: the animated models into the instanced buffers,
    // the world's texture animation stepped. The positions decayed to COMMON
    // since last frame (or the blood copies left them COPY_DEST); explicit, so
    // the transition to a build input below has a state to leave from.
    if (w.positions_instanced)
        transition(cmd, w.positions_instanced,
                   blood_wrote ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d3d12_timer_mark(cmd, &timers, PROFILER_INSTANCE_GEOMETRY, PROFILER_START);
    d3d12_record_instance_geometry(&p, w.prims_instanced ? frame->num_model_instances : 0,
                                   frame->animate_world && w.world_buffer ? ubo->num_static_primitives : 0);
    d3d12_timer_mark(cmd, &timers, PROFILER_INSTANCE_GEOMETRY, PROFILER_STOP);

    // vkpt_pt_create_all_dynamic, as d3d12_world_plan_dynamic laid it out (and
    // frontend.c put it in the instance list): the runs of the instanced
    // positions - a build input now - and the transparency geometry
    bool any_dynamic = false;
    for (int i = 0; i < D3D12_NUM_DYN; i++)
        any_dynamic |= fr->dyn_blases[i].address != 0;
    // (and with the fog, the shadow map's vertices)
    if (w.positions_instanced)
        transition(cmd, w.positions_instanced, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           (fog ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER : 0)));
    d3d12_timer_mark(cmd, &timers, PROFILER_BVH_UPDATE, PROFILER_START);
    if (any_dynamic && !d3d12_dynamic_blas_build(cmd, &fr->dyn, fr->dyn_blases, D3D12_NUM_DYN, fr->dyn_flags)) {
        // this frame's clustered BLASes were not built; the next ones are built whole
        w.clusters_failed = true;
        Com_EPrintf("D3D12: NVAPI refused the clustered BLAS builds - pt_clusters is off until the renderer restarts\n");
    }
    d3d12_timer_mark(cmd, &timers, PROFILER_BVH_UPDATE, PROFILER_STOP);
    if (fog) {
        d3d12_timer_mark(cmd, &timers, D3D12_PROFILER_SHADOW_MAP_DYNAMIC, PROFILER_START);
        shadow_map_dynamic(cmd, frame);
        d3d12_timer_mark(cmd, &timers, D3D12_PROFILER_SHADOW_MAP_DYNAMIC, PROFILER_STOP);
    }

    // vkpt_pt_create_toplevel's two TLASes: the list's first num_geometry_instances,
    // then the effects (explosions, particles, beams, sprites) after them - built
    // even when empty, so the effects rays find an empty structure, not old memory
    const uint32_t num_geometry = min(frame->num_geometry_instances, num_instances);
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS tlas_flags =
        frame->tlas_fast_trace ? TLAS_FAST_TRACE : TLAS_FAST_BUILD;
    const D3D12_GPU_VIRTUAL_ADDRESS upload_gpu = ID3D12Resource_GetGPUVirtualAddress(fr->upload);
    const D3D12_GPU_VIRTUAL_ADDRESS instances = upload_gpu + UPLOAD_TLAS_OFFSET;
    const D3D12_GPU_VIRTUAL_ADDRESS scratch = ID3D12Resource_GetGPUVirtualAddress(fr->tlas_scratch);
    d3d12_timer_mark(cmd, &timers, D3D12_PROFILER_BVH_TLAS, PROFILER_START);
    if (frame->ptlas && w.ptlas_route && fr->ptlas.held) {
        /* THE PARTITIONED TLAS: this slot's, updated in place with what changed since
           the slot's last frame - the world in the global partition, the dynamic
           BLASes (rebuilt every frame, maybe at the same address) rewritten always. */
        if (ptlas_diff(&fr->ptlas, frame->instances, num_geometry, min(frame->num_world_instances, num_geometry),
                       min(frame->first_dynamic_instance, num_geometry), d3d12_ptlas_flags(frame->tlas_fast_trace)))
            d3d12_ptlas_record(cmd, w.ptlas_route, &fr->ptlas, frame->tlas_fast_trace,
                               fr->upload_map + UPLOAD_PTLAS_OFFSET, upload_gpu + UPLOAD_PTLAS_OFFSET, fr->tlas,
                               scratch);
    } else {
        // over whatever a partitioned TLAS left at this address
        ptlas_invalidate(&fr->ptlas);
        d3d12_tlas_build(cmd, instances, num_geometry, tlas_flags, fr->tlas, scratch);
    }
    d3d12_tlas_build(cmd, instances + (uint64_t)num_geometry * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                     num_instances - num_geometry, tlas_flags, fr->tlas_effects,
                     scratch + ALIGN_UP(w.geometry_scratch, D3D12_AS_ALIGNMENT));
    d3d12_as_barrier(cmd, &fr->tlas_pool);
    d3d12_timer_mark(cmd, &timers, D3D12_PROFILER_BVH_TLAS, PROFILER_STOP);

    // NVIDIA Flow's passes (flow.c; Flow's simulation is ahead of this list): the
    // occupancy and the light sums, then the probes over this slot's TLAS
    {
        d3d12_flow_passes_t fp;
        p.flow_env_sets[0] = d3d12_flow_frame(&frame->flow, f, fr->tlas, &fp);
        p.flow_env_sets[1] = &w.vertex_buffer_set;
        p.flow_env_sets[2] = &fr->ubo_set;
        p.flow_env_sets[3] = &w.textures[odd];
        d3d12_record_flow(&p, &fp);
    }

    // decayed to COMMON since last frame; explicit, so the copy-out below has
    // a state to leave from
    transition(cmd, w.readback_buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // vkpt_physical_sky_record_cmd_buffer, ahead of the primary rays: the
    // envmap and the sun colour this frame's lighting reads
    d3d12_timer_mark(cmd, &timers, PROFILER_UPDATE_ENVIRONMENT, PROFILER_START);
    if (frame->sky_update && !frame->no_world) {
        d3d12_sky_pass_t s = {
            .cmd = cmd,
            .enhanced_barriers = d3d.caps.enhanced_barriers,
            .shadowmap_pso = w.sky_shadowmap_pso,
            .shadowmap = w.sky_shadowmap,
            .shadowmap_dsv = sky_dsv(),
            .grid_vertices = w.sky_grid_vertices,
            .grid_indices = w.sky_grid_indices,
            .grid_index_count = w.sky_grid_index_count,
            .sky_pso = frame->sky_space ? w.sky_space_pso : w.sky_pso,
            .resolve_pso = w.sky_resolve_pso,
            .sets = { &fr->ubo_set, &w.textures[odd], &w.vertex_buffer_set, &w.sky_set },
            .sun_color_buffer = w.sun_color_buffer,
            .sun_color_size = SUN_COLOR_SIZE,
            .zero_buffer = w.zero_buffer,
        };
        memcpy(s.viewproj, frame->sky_viewproj, sizeof(s.viewproj));
        d3d12_record_sky(&s);
    }
    d3d12_timer_mark(cmd, &timers, PROFILER_UPDATE_ENVIRONMENT, PROFILER_STOP);

    // the UBO's switches are vkpt's cvars (zeroed in pure photo mode, where
    // the shaders stand down)
    d3d12_passes_frame_t pf = {
        .field_width = frame->field_width,
        .slice_width = frame->slice_width,
        .render_width = frame->render_width,
        .render_height = frame->render_height,
        .taa_output_width = frame->taa_output_width,
        .taa_output_height = frame->taa_output_height,
        .taa_image_width = frame->extents.taa_width,
        .taa_image_height = frame->extents.taa_height,
        .enable_denoiser = frame->enable_denoiser,
        .reflect_refract = (int)ubo->pt_reflect_refract,    // ref_mode's: at least 4 in photo mode
        .num_bounce_rays = frame->num_bounce_rays,
        .caustics = frame->caustics,
        .restir_gi = ubo->pt_restir_gi != 0,
        .restir_gi_spatial = ubo->pt_restir_gi_spatial > 0,
        .tone_mapping = ubo->tm_enable != 0,
        .tm_reset = w.tm_reset,
        .frame_time = frame->frame_time,
        .tm_slope_blur_sigma = Cvar_Get("tm_slope_blur_sigma", "6.0", 0)->value,
        .tm_knee_start = Cvar_Get("tm_knee_start", "0.9", 0)->value,
        .tm_white_point = Cvar_Get("tm_white_point", "10.0", 0)->value,
        .hdr = d3d.hdr,
        .sharc = sharc,
        .sharc_clear = frame->sharc_clear || w.sharc_dirty,
        .sharc_stride = (uint32_t)max(1.f, min(16.f, ubo->pt_sharc_stride)),
        .bloom = frame->bloom,
        .bloom_argument_scale = frame->bloom_argument_scale,
        .bloom_normalization_scale = frame->bloom_normalization_scale,
        .bloom_num_samples = frame->bloom_num_samples,
        .bloom_debug = frame->bloom_debug,
        .dlss = frame->dlss,
        .dlss_bypass = frame->dlss_bypass,
        .dlss5 = frame->dlss5,
        .output_width = frame->extents.unscaled_width,
        .output_height = frame->extents.unscaled_height,
        .taa_folded = frame->taa_folded,
        .god_rays = fog,
        .god_rays_march = frame->god_rays_march,
        .fog_accum_march = frame->fog_accum_march,
        .froxel = fog && frame->froxel && w.froxel_made,
        .froxel_restir = frame->froxel_restir,
        .sky_exposure_first = frame->sky_exposure_first,
        .sky_exposure_count = frame->sky_exposure_count,
        .motion_blur = frame->motion_blur,
        .motion_blur_to_bloom = frame->motion_blur_to_bloom,
        .chromatic_aberration = frame->chromatic_aberration,
        .flow_split = frame->flow.split,
        .flow_reflect = frame->flow.reflect,
    };
    memcpy(pf.motion_blur_push, frame->motion_blur_push, sizeof(pf.motion_blur_push));
    memcpy(pf.chromatic_aberration_push, frame->chromatic_aberration_push, sizeof(pf.chromatic_aberration_push));
    memcpy(pf.dlss5_push, frame->dlss5_push, sizeof(pf.dlss5_push));
    d3d12_record_passes(&p, &pf);
    if (pf.tone_mapping)
        w.tm_reset = false;
    if (sharc)
        w.sharc_dirty = false;

    // the shaders' readback, for vkpt_readback when this slot comes round again
    transition(cmd, w.readback_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, fr->staging[D3D12_WORLD_STAGING_READBACK], 0, w.readback_buffer,
                                                0, sizeof(ReadbackBuffer));

    // on screen: TAA_OUTPUT (vkpt_final_blit_simple), DLSS_OUTPUT with DLSS, or
    // the d3d12_show image
    {
        const bool final = w.display_image == VKPT_IMG_TAA_OUTPUT;
        const d3d12_set_t *sets[2] = { &fr->ubo_set, final ? &w.textures[odd] : &w.display[odd] };

        // the picture's part of the image: the TAA images hold the output,
        // FLAT_* the interleaved render, the path tracer's images both
        // checkerboard fields side by side - field 0 is shown, stretched
        uint32_t src_w, src_h;
        const int shown = final && frame->dlss ? VKPT_IMG_DLSS_OUTPUT : w.display_image;
        image_made_extent(shown, &w.extents, &src_w, &src_h);
        const char *name = d3d12_image_name(shown);
        if (shown == VKPT_IMG_DLSS_OUTPUT) {
            src_w = min(src_w, frame->extents.unscaled_width);
            src_h = min(src_h, frame->extents.unscaled_height);
        } else if (final || !strncmp(name, "ASVGF_TAA_", 10)) {
            src_w = min(src_w, frame->taa_output_width);
            src_h = min(src_h, frame->taa_output_height);
        } else if (!strncmp(name, "FLAT_", 5) || !strncmp(name, "DLSS_", 5)
                   || shown == VKPT_IMG_PT_DLSS_MOTION) {
            // the combine pass's outputs (pt_dlss_debug's RR inputs): one image, not fields
            src_w = min(src_w, frame->render_width);
            src_h = min(src_h, frame->render_height);
        } else {
            src_w = min(src_w, frame->field_width);
            src_h = min(src_h, frame->render_height);
        }

        blit_push_t pc = { 0 };
        // TEX_TAA_OUTPUT - which the display sets point at the image - or TEX_DLSS_OUTPUT
        pc.source = shown == VKPT_IMG_DLSS_OUTPUT ? 1 : 0;
        pc.src_width = (int32_t)src_w;
        pc.src_height = (int32_t)src_h;
        pc.out_width = d3d.width;
        pc.out_height = d3d.height;
        pc.seed = (uint32_t)d3d.frame_counter;
        // grain and the CRT: on the picture, not on a d3d12_show view (as vkpt's
        // final_blit_sources); kept for the frames FG makes from this one
        w.blit_effects = final && frame->blit_effects;
        if (w.blit_effects)
            memcpy(&w.blit_fx, frame->blit_effects_push, sizeof(w.blit_fx));
        record_blit(cmd, sets, &pc, d3d12_rtv(D3D12_RTV_FRAME_TARGET), w.blit_effects);
    }

    w.last_drawn = true;
    w.last_slot = f;
    w.last_odd = odd;
    w.last_output_width = frame->extents.unscaled_width;
    w.last_output_height = frame->extents.unscaled_height;
    return true;
}

/* FRAME GENERATION, after d3d12_world_render (frontend.c's d3d12_frontend_dlssg_record
   drives it): the frame's colour encode and decode around DLSS-G's evaluations, and each
   generated frame's way to the screen. Both use the sets of the frame just drawn, and
   set this renderer's heaps again: an evaluation leaves NGX's on the list. */
void d3d12_world_dlssg_convert(ID3D12GraphicsCommandList4 *cmd, uint32_t mode, uint32_t target)
{
    if (!w.last_drawn)
        return;
    ID3D12DescriptorHeap *heaps[2] = { d3d.srv_heap, d3d.sampler_heap };
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cmd, 2, heaps);
    // after the blit's read of DLSS_OUTPUT, or NGX's writes
    d3d12_barrier_all(cmd, d3d.caps.enhanced_barriers);
    world_frame_t *fr = &w.frame[w.last_slot];
    const d3d12_passes_t p = {
        .cmd = cmd,
        .enhanced_barriers = d3d.caps.enhanced_barriers,
        .psos = w.passes,
        .standard_sets = { &fr->ubo_set, &w.textures[w.last_odd], &w.vertex_buffer_set },
        .heaps = heaps,
        .num_heaps = 2,
    };
    d3d12_record_dlssg_convert(&p, mode, target, w.last_output_width, w.last_output_height);
}

// vkpt_final_blit_simple(GetDLSSGImage(index)): DLSS_FG_OUTPUT `index` (1..5) on screen
void d3d12_world_blit_generated(ID3D12GraphicsCommandList4 *cmd, unsigned int index, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    if (!w.last_drawn || index < 1 || index > 5)
        return;
    ID3D12DescriptorHeap *heaps[2] = { d3d.srv_heap, d3d.sampler_heap };
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cmd, 2, heaps);
    // after NGX's writes (or the decode's)
    d3d12_barrier_all(cmd, d3d.caps.enhanced_barriers);
    world_frame_t *fr = &w.frame[w.last_slot];
    const d3d12_set_t *sets[2] = { &fr->ubo_set, &w.textures[w.last_odd] };

    uint32_t src_w, src_h;
    d3d12_image_extent(VKPT_IMG_DLSS_FG_OUTPUT, &w.extents, &src_w, &src_h);
    blit_push_t pc = { 0 };
    pc.source = 1 + (int32_t)index;     // final_blit_fetch: 2..6 are TEX_DLSS_FG_OUTPUT..5
    pc.src_width = (int32_t)min(src_w, w.last_output_width);
    pc.src_height = (int32_t)min(src_h, w.last_output_height);
    pc.out_width = d3d.width;
    pc.out_height = d3d.height;
    pc.seed = (uint32_t)(d3d.frame_counter * 8 + index);    // one per presented frame
    record_blit(cmd, sets, &pc, rtv, w.blit_effects);
}
