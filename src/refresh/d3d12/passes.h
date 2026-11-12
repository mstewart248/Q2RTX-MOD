/*
THE 3D VIEW'S COMPUTE PASSES, recorded in R_RenderFrame_RTX's order: the
primary rays, the fog (god_rays.c: its march, the froxel grid, the filter,
around the first reflection bounce), A-SVGF's gradient reprojection,
vkpt_pt_trace_lighting's passes, then the post chain (A-SVGF or plain
compositing, the checkerboard interleave, TAA, tone mapping). Each pass is the vkpt shader of that name, translated
(spv2dxil), with vkpt's dispatch sizes and push constants.

Like bindings.c and accel.c this file includes no engine header: world.c
records the game's frame with it, and tests/d3d12_world_test.c includes it to
record exactly the same frame headlessly. The frame's sizes and switches come
in as plain values (d3d12_passes_frame_t), worked out by world.c from what
vkpt_prepare_frame left.

SYNCHRONISATION. vkpt orders each pass's writes before the next one's reads
with BARRIER_COMPUTE on the images involved. The G-buffer images here are
simultaneous-access textures and everything else is a buffer, so a pass needs
no transitions, only an execution and memory barrier - one global barrier
after every dispatch (d3d12_barrier_all), which orders everything.
*/

#ifndef REFRESH_D3D12_PASSES_H
#define REFRESH_D3D12_PASSES_H

#include "bindings.h"
#include "../vkpt/profiler.h"   // PROFILER_*: vkpt's profiler rows, timed here too

typedef enum {
    D3D12_PASS_PRIMARY_RAYS,
    D3D12_PASS_REFLECT_REFRACT_FIRST,       // glass, water, mirrors: the first bounce
    D3D12_PASS_REFLECT_REFRACT_SECOND,      // ...and the later ones
    D3D12_PASS_DIRECT_LIGHTING,
    D3D12_PASS_DIRECT_LIGHTING_CAUSTICS,
    D3D12_PASS_INDIRECT_LIGHTING_FIRST,
    D3D12_PASS_INDIRECT_LIGHTING_SECOND,    // the second and later bounces
    D3D12_PASS_RESTIR_GI,
    D3D12_PASS_GRADIENT_REPROJECT,
    D3D12_PASS_GRADIENT_IMG,
    D3D12_PASS_GRADIENT_ATROUS,
    D3D12_PASS_TEMPORAL,
    D3D12_PASS_ATROUS_LF,
    D3D12_PASS_ATROUS_0,                    // A-SVGF's spatial iterations (spec_iteration 0..3)
    D3D12_PASS_ATROUS_1,
    D3D12_PASS_ATROUS_2,
    D3D12_PASS_ATROUS_3,
    D3D12_PASS_COMPOSITING,
    D3D12_PASS_INTERLEAVE,
    D3D12_PASS_TAAU,
    D3D12_PASS_TM_HISTOGRAM,
    D3D12_PASS_TM_CURVE,
    D3D12_PASS_TM_APPLY,
    D3D12_PASS_TM_APPLY_HDR,                // ...for an scRGB swapchain (vid_hdr; spec_tone_mapping_hdr 1)
    D3D12_PASS_INSTANCE_GEOMETRY,           // the animated models into the instanced buffers
    D3D12_PASS_ANIMATE_MATERIALS,           // the world's texture animation, in its primitives
    D3D12_PASS_SHARC_UPDATE,                // the radiance cache: trained from the primary surfaces
    D3D12_PASS_SHARC_RESOLVE,               // ...and folded into its resolved entries
    D3D12_PASS_BLOOM_DOWNSCALE,             // bloom.c: TAA_OUTPUT to a quarter
    D3D12_PASS_BLOOM_BLUR,                  // ...blurred across, then down
    D3D12_PASS_BLOOM_COMPOSITE,             // ...and blended back into TAA_OUTPUT
    D3D12_PASS_DLSSG_HDR,                   // dlssg_hdr.c: frame generation's colour encode/decode
    // god_rays.c's, on its layout: the god rays set, the vertex buffer, the UBO, the textures
    D3D12_PASS_GOD_RAYS,                    // the fog's march (push: 0 after the primary rays, 1 after the first bounce)
    D3D12_PASS_GOD_RAYS_FILTER,             // ...its filter, compositing the fog into PT_TRANSPARENT
    D3D12_PASS_FROXEL_RESERVOIR,            // the froxel grid's light selection (pt_fog_restir): candidates
    D3D12_PASS_FROXEL_SPATIAL,              // ...and the neighbours' reuse
    D3D12_PASS_SKY_EXPOSURE_BAKE,           // a batch of pt_fog_sky_exposure's bake (push: its first voxel)
    D3D12_PASS_FROXEL_SCATTER,              // the grid's in-scatter, a thread per cell
    D3D12_PASS_FROXEL_INTEGRATE,            // ...summed front to back, a thread per column
    D3D12_PASS_MOTION_BLUR,                 // motion_blur.c: the frame into MOTION_BLUR
    D3D12_PASS_CHROMATIC_ABERRATION,        // chromatic_aberration.c: the same
    D3D12_PASS_POSTFX_COPY,                 // d3d12/shader/postfx_copy.comp: MOTION_BLUR back over the frame
    D3D12_PASS_CRT_DOWNSAMPLE,              // draw.c's CRT grid into MOTION_BLUR (ubo + textures; world.c's blit)
    D3D12_PASS_DLSS5_CONVERT,               // DLSS 5's encode and resolve around the model (vkpt/DLSSNR.c)
    // NVIDIA Flow (vkpt/flow.c, path_tracer.c)
    D3D12_PASS_PRIMARY_RAYS_FLOW_SPLIT,     // pt_flow_split_pass: the primary rays without the Flow march
    D3D12_PASS_FLOW_MARCH,                  // ...and the march as a pass of its own
    D3D12_PASS_REFLECT_REFRACT_FIRST_FLOW,  // the bounces with the march, on a frame with a Flow grid
    D3D12_PASS_REFLECT_REFRACT_SECOND_FLOW,
    D3D12_PASS_FLOW_OCCUPANCY,              // flow_occupancy.comp: the bricks the march samples (ubo + textures)
    D3D12_PASS_FLOW_LIGHTS,                 // flow_lights.comp: the glow round each light slot, for the host
    D3D12_PASS_FLOW_AMBIENT,                // flow_ambient.comp: the world's light on the smoke (flow env layout)
    D3D12_NUM_PASSES
} d3d12_pass_t;

typedef struct {
    d3d12_pipeline_layout_id_t  layout;
    const char                  *shader;    // baseq2/shader_d3d12/<shader>.dxil
} d3d12_pass_info_t;

// The DXIL behind each pass: the specialization variants are cmake's
// compile_shader_d3d12_variant files, named after their values.
extern const d3d12_pass_info_t d3d12_pass_info[D3D12_NUM_PASSES];

/* THE PROFILER (profiler 1, profiler_dump; gpu_timers.c): vkpt's entries
   (vkpt/profiler.h), a timestamp pair each at entry * 2 + PROFILER_START/STOP, marked
   where vkpt marks them. After them, the second pieces of three spans vkpt times in one
   piece and this renderer records in two: each is timed apart and added to its entry. */
enum {
    // the sun's shadow map's animated models, after their BLAS builds (world.c's
    // shadow_map_dynamic; the static half is at the head of the list): + SHADOW_MAP
    D3D12_PROFILER_SHADOW_MAP_DYNAMIC = NUM_PROFILER_ENTRIES,
    // the TLAS builds, after that (the dynamic BLASes are BVH_UPDATE's): + BVH_UPDATE
    D3D12_PROFILER_BVH_TLAS,
    // Flow's occupancy and light sums, which vkpt submits right after Flow's own work
    // and which are in the frame's list here (d3d12_record_flow): + FLOW_SIMULATION
    D3D12_PROFILER_FLOW_PASSES,
    D3D12_NUM_PROFILER_ENTRIES
};
#define D3D12_PROFILER_QUERIES  (D3D12_NUM_PROFILER_ENTRIES * 2)

// a frame slot's timestamp queries (heap NULL: no profiler)
typedef struct {
    ID3D12QueryHeap     *heap;
    uint32_t            base;       // the slot's first query
    bool                *used;      // [D3D12_PROFILER_QUERIES]: the ones written, for the resolve
} d3d12_timers_t;

// vkpt's BEGIN_PERF_MARKER / END_PERF_MARKER: entry's timestamp (action: PROFILER_START
// or PROFILER_STOP) into cmd, when there are timers
void d3d12_timer_mark(ID3D12GraphicsCommandList4 *cmd, const d3d12_timers_t *t, int entry, int action);

// buffer clears copy from this many zeros (there is no CPU-only descriptor
// heap for ClearUnorderedAccessViewUint)
#define D3D12_ZERO_BUFFER_SIZE  (1u << 20)

typedef struct {
    ID3D12GraphicsCommandList4  *cmd;               // descriptor heaps already set
    bool                        enhanced_barriers;
    ID3D12PipelineState *const  *psos;              // [D3D12_NUM_PASSES]
    const d3d12_set_t           *pt_sets[5];        // rt, ubo, textures, vertex buffer, tiles
    const d3d12_set_t           *standard_sets[3];  // ubo, textures, vertex buffer
    // god_rays.c's layout: this frame slot's god rays set, vertex buffer, ubo, textures
    // (NULL: no fog)
    const d3d12_set_t           *god_rays_sets[4];
    // flow.c's probe pass's layout: this frame slot's flow env set, vertex buffer, ubo,
    // textures (d3d12_record_flow; NULL: no probes)
    const d3d12_set_t           *flow_env_sets[4];
    ID3D12Resource              *tonemap_buffer;    // ToneMappingBuffer, in COMMON
    uint64_t                    tonemap_size;
    ID3D12Resource              *zero_buffer;       // D3D12_ZERO_BUFFER_SIZE zeros
    // the radiance cache (pt_sharc): keys, accumulators, resolved entries -
    // vertex_buffer.c's, in COMMON - and SHARC_CAPACITY
    ID3D12Resource              *sharc_buffers[3];
    uint64_t                    sharc_sizes[3];
    uint32_t                    sharc_entries;
    // DLSS (the frame's `dlss`): records NGX's evaluation of TAA_OUTPUT into DLSS_OUTPUT
    // (frontend.c's, or the test's). NGX leaves its own heaps and root signature on the
    // list, so these heaps are set again after it.
    void                        (*upscale)(void *ctx, ID3D12GraphicsCommandList4 *cmd);
    void                        *upscale_ctx;
    // DLSS 5 (the frame's `dlss5`; frontend.c's), with upscale_ctx: stage 0 makes its
    // feature if it must (false: DLSS 5 does not run this frame), stage 1 records the
    // evaluation (false: it failed - no resolve). Each is followed as the upscale is.
    bool                        (*neural)(void *ctx, ID3D12GraphicsCommandList4 *cmd, int stage);
    ID3D12DescriptorHeap *const *heaps;
    uint32_t                    num_heaps;
    // TILE COMPACTION (pt_tile_compaction; tile_buffer NULL = every bounce over the whole
    // grid): the lists, the tiles set's buffer (COMMON between command lists); an upload
    // buffer with this view's header in d3d12_tile_header's form and zeroed masks after
    // it, tile_reset_size bytes copied over the lists' head first; the 12 bytes a
    // bounce's DISPATCH is copied to; and a DISPATCH-only command signature
    ID3D12Resource              *tile_buffer;
    ID3D12Resource              *tile_reset;
    uint32_t                    tile_reset_size;
    ID3D12Resource              *tile_args;
    ID3D12CommandSignature      *tile_dispatch;
    // SER (pt_ser 1, 2): the indirect lighting bounces as ray generation shaders -
    // [the first bounce, the later ones], each a state object and its raygen record
    // (NULL: the compute pass, as always)
    ID3D12StateObject           *raygen[2];
    D3D12_GPU_VIRTUAL_ADDRESS   raygen_records[2];
    // the profiler's timestamps (heap NULL: none)
    d3d12_timers_t              timers;
} d3d12_passes_t;

// the post effects' push constants, in words: vkpt.h's vkpt_motion_blur_push_t,
// vkpt_chromatic_aberration_push_t and vkpt_final_blit_push_t, DLSSNR.h's
// dlss5_convert_push_t (frontend.c and world.c check the sizes)
#define D3D12_MOTION_BLUR_PUSH_WORDS            17
#define D3D12_CHROMATIC_ABERRATION_PUSH_WORDS   6
#define D3D12_BLIT_PUSH_WORDS                   24
#define D3D12_DLSS5_PUSH_WORDS                  8

typedef struct {
    uint32_t    field_width;        // one checkerboard field (vkpt_pt_field_width)
    uint32_t    slice_width;        // both fields (qvk.gpu_slice_width)
    uint32_t    render_width, render_height;
    uint32_t    taa_output_width, taa_output_height;
    uint32_t    taa_image_width, taa_image_height;
    bool        enable_denoiser;    // reference mode: A-SVGF, else plain compositing
    int         reflect_refract;    // the UBO's pt_reflect_refract: reflection / refraction bounces
    float       num_bounce_rays;    // 0, 0.5 (half height), 1..8
    bool        paths;              // pt_bounce_cull: the later bounces walk the path lists (needs tile_buffer)
    bool        sparse;             // pt_sparse with its path list: direct lighting fills it, then it and the first bounce walk it
    bool        caustics;           // pt_caustics
    bool        restir_gi;          // the UBO's pt_restir_gi
    bool        restir_gi_spatial;  // the UBO's pt_restir_gi_spatial > 0
    bool        tone_mapping;       // the UBO's tm_enable
    bool        tm_reset;           // start the histogram and the eye adaptation afresh
    float       frame_time;         // seconds
    float       tm_slope_blur_sigma, tm_knee_start, tm_white_point;     // the cvars
    bool        hdr;                // d3d.hdr: tone map for the scRGB swapchain
    bool        sharc;              // vkpt_frame_prep_t's sharc_active (the UBO's pt_sharc agrees)
    bool        sharc_clear;        // ...its sharc_clear: zero the cache first
    uint32_t    sharc_stride;       // pt_sharc_stride, [1..16]: one training path per stride^2 pixels
    bool        bloom;              // vkpt_bloom_will_run: bloom_enable, or the menu's / wheel's blur
    float       bloom_argument_scale, bloom_normalization_scale;    // vkpt_bloom_blur_params
    int32_t     bloom_num_samples;
    // bloom_debug 1..3: the downscale, the horizontal blur or the whole blur stretched
    // over the frame in place of the composite (POSTFX_COPY's modes 3 and 4)
    int32_t     bloom_debug;
    // motion_blur.c's and chromatic_aberration.c's passes, after the upscale with DLSS,
    // before bloom, on the image the picture is in: each into MOTION_BLUR, then copied
    // back (POSTFX_COPY) - unless bloom's downscale takes motion blur's result back
    // itself (motion_blur_to_bloom, vkpt_motion_blur_bloom_takes_copy). The push
    // constants are the shaders', from vkpt_motion_blur_params / _chromatic_aberration_.
    bool        motion_blur;
    bool        motion_blur_to_bloom;
    uint32_t    motion_blur_push[D3D12_MOTION_BLUR_PUSH_WORDS];
    bool        chromatic_aberration;
    uint32_t    chromatic_aberration_push[D3D12_CHROMATIC_ABERRATION_PUSH_WORDS];
    // DLSSEnabled: TAA_OUTPUT (at the render size) upscaled into DLSS_OUTPUT, and bloom
    // and tone mapping on that at the output size - vkpt's DLSS arm
    bool        dlss;
    bool        dlss_bypass;        // photo mode: TAA_OUTPUT copied into DLSS_OUTPUT, no NGX
    uint32_t    output_width, output_height;    // qvk.extent_unscaled
    // DLSS 5 Neural Rendering on DLSS_OUTPUT after tone mapping (R_EndFrame_RTX's
    // DLSS5Apply): dlss5_convert.comp's encode, the `neural` callback's evaluation into
    // MOTION_BLUR, the resolve - [0] the encode's push constants, [1] the resolve's
    bool        dlss5;
    uint32_t    dlss5_push[2][D3D12_DLSS5_PUSH_WORDS];
    bool        taa_folded;         // vkpt_taa_folded: the interleave pass writes TAA_OUTPUT, no TAA
    // the fog (vkpt_frame_prep_t's): the march, the froxel grid and the filter run at all
    // (god_rays_enabled, and the god rays sets given); the march (not god_rays_march_discarded);
    // a ray per pixel rather than per 2x2 block (photo mode's fog_accum_march); the grid
    // (run_froxel) with its light selection (pt_fog_restir); a batch of the sky exposure
    // bake (vkpt_sky_exposure_next_batch's; count 0 = none)
    bool        god_rays;
    bool        god_rays_march;
    bool        fog_accum_march;
    bool        froxel;
    bool        froxel_restir;
    uint32_t    sky_exposure_first, sky_exposure_count;
    // NVIDIA Flow, path_tracer.c's choices: the march as its own pass after the primary
    // rays (pt_flow_split_pass, on a frame with a grid), and the reflect/refract bounces
    // with the march compiled in (a grid, or pt_reflect_noflow 0)
    bool        flow_split;
    bool        flow_reflect;
} d3d12_passes_frame_t;

// vkpt's BARRIER_COMPUTE on everything: one global barrier, or a UAV barrier
// on everything without enhanced barriers
void d3d12_barrier_all(ID3D12GraphicsCommandList4 *cmd, bool enhanced);

// vkCmdFillBuffer(0) for a DEFAULT buffer in COMMON (every buffer is, between
// command lists), as copies from `zeros`. Leaves it in UNORDERED_ACCESS: a
// buffer cannot be promoted out of COPY_DEST.
void d3d12_clear_buffer(ID3D12GraphicsCommandList4 *cmd, ID3D12Resource *buffer, uint64_t size,
                        ID3D12Resource *zeros);

// The primary rays through tone mapping (TAA_OUTPUT holds the picture, or with
// DLSS DLSS_OUTPUT), each pass followed by a barrier.
void d3d12_record_passes(const d3d12_passes_t *p, const d3d12_passes_frame_t *f);

/* NVIDIA Flow's passes (vkpt/flow.c's, with vkpt_flow_dispatch_t's counts), after the
   TLAS is built and before the primary rays - Flow's own work went ahead of the list:
   - the occupancy's group bitset cleared, then flow_occupancy.comp marks the bricks
     of each block that the march samples;
   - flow_lights.comp adds up the glow round each light slot, and this set's sums are
     copied to the readback buffer the host reads once the frame's fence has passed;
   - flow_ambient.comp lights the probe grid from the world (p->flow_env_sets).
   The buffers are in COMMON at the start, as between command lists; each pass is
   followed by a barrier. A count of 0 (or a missing buffer) skips its pass. */
typedef struct {
    ID3D12Resource  *occupancy;             // both sets' (flow.c's occupancy_stride apart)
    uint64_t        occupancy_offset;       // this set's: a uvec4 a block,
    uint32_t        occupancy_capacity;     // ...for this many blocks (flow.c's occupancy_blocks),
    uint32_t        group_bitset_size;      // ...then the group and block bitsets, this many bytes
    uint32_t        occupancy_blocks;       // the pass's groups, at most the capacity
    ID3D12Resource  *light_sums;            // both sets' (DEFAULT, written by the pass)
    ID3D12Resource  *light_sums_readback;   // ...and the host's copy (READBACK)
    uint64_t        light_sums_offset;      // this set's slice, in both
    uint64_t        light_sums_size;
    uint32_t        light_blocks;           // the pass's groups
    uint32_t        env_groups[3];          // the probe pass's groups
} d3d12_flow_passes_t;

void d3d12_record_flow(const d3d12_passes_t *p, const d3d12_flow_passes_t *f);

// TILE COMPACTION: the game's rows of groups (passes.c says why rows), and so the most
// tiles one list's dispatch can hold (vkpt_pt_tile_header's max_groups)
#define D3D12_TILE_ROW_GROUPS   256u
#define D3D12_TILE_MAX_GROUPS   (D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * D3D12_TILE_ROW_GROUPS)

// vkpt_pt_tile_header's header (PT_TILE_HDR_WORDS words, in place) in the form these
// passes dispatch: each list's words { 0, row_groups, 0, 1 } (shader/constants.h)
void d3d12_tile_header(uint32_t *header, uint32_t row_groups);

// vkpt_instance_geometry, before the dynamic BLASes are built: instance_geometry.comp,
// one group per animated model instance (vkpt's upload_info.num_instances), writes the
// instanced primitives and positions; then, when the world's texture animation steps
// this frame, animate_materials.comp over its first `num_world_prims` primitives (the
// UBO's num_static_primitives; 0 = not this frame). Each followed by a barrier.
void d3d12_record_instance_geometry(const d3d12_passes_t *p, uint32_t num_instances, uint32_t num_world_prims);

/* vkpt_dlssg_hdr_convert: dlssg_hdr.comp over width x height of one image - `target` 0
   DLSS_OUTPUT, 1..5 DLSS_FG_OUTPUT..5 - in place: `mode` 2 encodes linear to sRGB for
   frame generation, 3 decodes it back (0 and 1 are the HDR10 pair). Then a barrier. */
void d3d12_record_dlssg_convert(const d3d12_passes_t *p, uint32_t mode, uint32_t target, uint32_t width,
                                uint32_t height);

/* shadow_map.c's vkpt_shadow_map_render, its draws: shadow_map.vert over float3
   positions (the sky's pipeline, sky.h), into one layer of the sun's shadow map,
   each draw with its matrix as the push constant (the view-projection, times a
   model's own for an instance). The map is in DEPTH_WRITE; `clear` sets the
   layer to 1 - nothing shadowed - first. */
typedef struct {
    D3D12_VERTEX_BUFFER_VIEW    vertices;
    uint32_t                    first, count;   // vertices
    float                       mvp[16];        // column-major
} d3d12_shadow_draw_t;

void d3d12_record_shadow_map(ID3D12GraphicsCommandList4 *cmd, ID3D12PipelineState *pso,
                             D3D12_CPU_DESCRIPTOR_HANDLE layer, uint32_t size, bool clear,
                             const d3d12_shadow_draw_t *draws, uint32_t num_draws);

// blood.c's regions (vkpt_blood_region_t), in primitives: in its shadow copy and
// this frame's staging, in the instanced buffers
typedef struct {
    uint32_t                    src, dst, count;
} d3d12_blood_region_t;

typedef struct {
    ID3D12Resource              *prims, *positions;     // the instanced buffers, COMMON
    ID3D12Resource              *staging_prims, *staging_positions;     // NULL: the zeroing alone
    uint32_t                    staging_capacity;       // primitives
    uint32_t                    prim_size, position_size;   // bytes a primitive: VboPrimitive, 9 floats
    ID3D12Resource              *zeros;                 // D3D12_ZERO_BUFFER_SIZE zeros
    uint32_t                    first, end;             // the section, clipped to the buffers
    bool                        zero_section;           // an epoch frame: the device's copy is void
    const d3d12_blood_region_t  *regions;               // ascending, disjoint
    uint32_t                    num_regions;
} d3d12_blood_upload_t;

// vkpt_blood_update's device half. On an epoch frame the section is zeroed -
// only between the regions, so no two copies write the same bytes and none has
// to wait for another - then each region is copied from the staging pair. A
// region outside [first, end) or past the staging is skipped (and zeroed, on an
// epoch frame). True when anything was written: both buffers are COPY_DEST then.
bool d3d12_record_blood_upload(ID3D12GraphicsCommandList4 *cmd, const d3d12_blood_upload_t *u);

#endif // REFRESH_D3D12_PASSES_H
