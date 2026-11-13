/*
THE D3D12 RTX RENDERER (vid_rtx 3).

A second backend for the path tracer, beside the Vulkan one in src/refresh/vkpt.
See ROADMAP.md in this directory for what works, what is next and why it is
built this way. In one paragraph:

  * SHADERS are not ported. src/refresh/vkpt/shader stays the only copy; the
    build compiles it a second time with -DVKPT_D3D12 and spv2dxil/ turns that
    SPIR-V into signed DXIL in baseq2/shader_d3d12. Every shader change reaches
    both renderers.
  * THE FRONTEND is not ported either. Everything in vkpt that does not touch
    Vulkan - the 2D queue, model loading, materials, (next) the BSP mesh, light
    lists and the UBO - is called from here through frontend.c, the only file in
    this directory that includes vkpt.h. What this directory owns is the GPU
    side: device, memory, descriptors, pipelines, acceleration structures,
    command recording, presentation and (later) DLSS through NGX's D3D12 path.

Ray query only: the HLSL route has no ray tracing pipeline stages, and on this
renderer ray query is the faster mode anyway (Vulkan measurements, 09-23).
*/

#ifndef REFRESH_D3D12PT_H
#define REFRESH_D3D12PT_H

#include "shared/shared.h"
#include "common/bsp.h"
#include "common/cmd.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/files.h"
#include "common/math.h"
#include "client/video.h"
#include "client/client.h"
#include "refresh/refresh.h"
#include "refresh/images.h"
#include "refresh/models.h"

#define COBJMACROS
#include <windows.h>
#ifdef D3D12PT_DEFINE_GUIDS
// device.c only: defines every IID/CLSID the headers below declare, the
// Agility SDK's preview interfaces included, which dxguid.lib does not have.
#include <initguid.h>
#endif
#include <d3d12.h>
#include <dxgi1_6.h>

/* The binding rule, vkpt's set layouts and the root signatures made from
   them (no engine headers: the headless tests share it). */
#include "bindings.h"
/* BLAS / TLAS builds and their memory (also engine-free) */
#include "accel.h"
#include "passes.h"

/* ------------------------------------------------------------------------- */
/* DEVICE                                                                     */
/* ------------------------------------------------------------------------- */

#define D3D12_FRAMES_IN_FLIGHT          2
#define D3D12_SWAPCHAIN_IMAGES          3
// frame generation's swapchain (swapchain.c's d3d12_fg_mode), on a present queue of
// its own, has a buffer more; its present sources come in D3D12_FG_SETS sets of the
// real frame and up to D3D12_FG_MAX_GENERATED generated ones (DLSSG_MAX_GENERATED_FRAMES)
#define D3D12_FG_SWAPCHAIN_IMAGES       4
#define D3D12_FG_SETS                   2
#define D3D12_FG_MAX_GENERATED          5

/* The shader-visible CBV/SRV/UAV heap: the bindless images first, one slot per
   image handle (so a qhandle_t is its own descriptor index, as on Vulkan) in each of
   two copies, one per frame parity (textures.c, d3d12_bindless_base), then
   a ring of per-frame descriptors, then the descriptor set instances
   (bindings.c: the textures sets, the UBO set, the vertex buffer set, ...). */
#define D3D12_BINDLESS_COPIES           2       // d3d.frame_counter & 1
#define D3D12_HEAP_BINDLESS_BASE        0
#define D3D12_HEAP_BINDLESS_COUNT       (MAX_RIMAGES * D3D12_BINDLESS_COPIES)
#define D3D12_HEAP_RING_BASE            (D3D12_HEAP_BINDLESS_BASE + D3D12_HEAP_BINDLESS_COUNT)
#define D3D12_HEAP_RING_PER_FRAME       4096
#define D3D12_HEAP_SETS_BASE            (D3D12_HEAP_RING_BASE + D3D12_HEAP_RING_PER_FRAME * D3D12_FRAMES_IN_FLIGHT)
#define D3D12_HEAP_SETS_COUNT           16384
#define D3D12_HEAP_SIZE                 (D3D12_HEAP_SETS_BASE + D3D12_HEAP_SETS_COUNT)

/* The shader-visible sampler heap: the four global samplers (the 2D layer reads
   them here), then set instances' sampler blocks. 2048 is the most a
   shader-visible sampler heap may hold. */
#define D3D12_SAMPLER_HEAP_SETS_BASE    D3D12_NUM_GLOBAL_SAMPLERS
#define D3D12_SAMPLER_HEAP_SIZE         2048

typedef struct {
    // what the device says it can do; d3d12_info prints all of it
    char                    adapter_name[128];
    LUID                    adapter_luid;
    uint64_t                dedicated_vram;
    uint64_t                driver_version;     // UMD version, 4 x 16 bits
    UINT                    sdk_version;        // the Agility runtime that loaded
    bool                    preview_sdk;
    bool                    process_sdk;        // ...through the D3D12SDKVersion export: NGX starts on it
    D3D_FEATURE_LEVEL       feature_level;
    D3D_SHADER_MODEL        shader_model;
    D3D12_RAYTRACING_TIER   raytracing_tier;    // 1.1 = ray query, 1.2 = + SER + OMM, 2.0 = + clusters
    D3D12_RESOURCE_BINDING_TIER binding_tier;
    D3D12_MESH_SHADER_TIER  mesh_shader_tier;
    D3D12_VARIABLE_SHADING_RATE_TIER vrs_tier;
    D3D12_WORK_GRAPHS_TIER  work_graphs_tier;
    D3D12_SAMPLER_FEEDBACK_TIER sampler_feedback_tier;
    D3D12_WAVE_MMA_TIER     wave_mma_tier;
    int                     linear_algebra_tier;    // cooperative vectors (preview SDK)
    bool                    clusters_ptlas;         // clustered geometry + partitioned TLAS (preview SDK)
    bool                    enhanced_barriers;
    bool                    gpu_upload_heap;        // CPU-writable VRAM (ReBAR)
    bool                    tearing;                // DXGI_FEATURE_PRESENT_ALLOW_TEARING
} d3d12_caps_t;

typedef struct {
    ID3D12CommandAllocator  *allocator;
    uint64_t                fence_value;        // signalled when this frame's work is done
    UINT                    ring_next;          // next free descriptor in this frame's ring slice
} d3d12_frame_t;

typedef struct {
    bool                    initialized;
    IDXGIFactory6           *factory;
    IDXGIAdapter4           *adapter;
    ID3D12DeviceFactory     *device_factory;    // keeps the chosen Agility runtime loaded
    ID3D12Device5           *device;            // Device5: ray tracing
    ID3D12CommandQueue      *queue;
    ID3D12GraphicsCommandList4 *cmd;            // one direct list, re-recorded every frame
    ID3D12Fence             *fence;
    HANDLE                  fence_event;
    uint64_t                fence_last_signalled;

    d3d12_frame_t           frames[D3D12_FRAMES_IN_FLIGHT];
    int                     frame_index;        // which of frames[] is being recorded
    uint64_t                frame_counter;
    bool                    frame_open;         // between R_BeginFrame and R_EndFrame

    ID3D12DescriptorHeap    *srv_heap;          // shader visible, D3D12_HEAP_SIZE
    ID3D12DescriptorHeap    *sampler_heap;      // shader visible, D3D12_SAMPLER_HEAP_SIZE
    ID3D12DescriptorHeap    *rtv_heap;          // CPU only: D3D12_RTV_* below
    UINT                    srv_increment, sampler_increment, rtv_increment;

    // swapchain
    HWND                    hwnd;
    IDXGISwapChain3         *swapchain;
    HANDLE                  swapchain_waitable;
    ID3D12Resource          *backbuffers[D3D12_FG_SWAPCHAIN_IMAGES];
    int                     swapchain_images;   // of backbuffers[]: 3, or 4 with frame generation
    int                     width, height;      // of the swapchain
    bool                    resize_pending;
    bool                    tearing_enabled;
    bool                    hdr;                // vid_hdr: the swapchain is scRGB (D3D12_HDR_FORMAT)

    /* THE FRAME TARGET. Every frame is composed here and copied to the
       backbuffer just before the present: a flip-discard backbuffer's contents
       are undefined once presented, so screenshots (IMG_ReadPixels) read this
       instead - and the path tracer's final image will land here too. Same
       format as the backbuffer, so the copy is a CopyResource; the render
       target view is sRGB, as vkpt's swapchain view is, so the shaders write
       linear values. Between frames it sits in COPY_SOURCE. */
    ID3D12Resource          *frame_target;

    d3d12_caps_t            caps;
} d3d12_state_t;

extern d3d12_state_t d3d;

#define D3D12_SWAPCHAIN_FORMAT          DXGI_FORMAT_B8G8R8A8_UNORM
#define D3D12_FRAME_TARGET_RTV_FORMAT   DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
// vid_hdr: vkpt's scRGB - linear FP16 with the sRGB primaries, 1.0 = 80 nits
// (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) - the swapchain, the frame target and
// its view alike
#define D3D12_HDR_FORMAT                DXGI_FORMAT_R16G16B16A16_FLOAT

// the frame target's view (and the present sources'): what every pipeline drawing
// into them is made for
static inline DXGI_FORMAT d3d12_frame_rtv_format(void)
{
    return d3d.hdr ? D3D12_HDR_FORMAT : D3D12_FRAME_TARGET_RTV_FORMAT;
}
enum {
    D3D12_RTV_FRAME_TARGET,
    // frame generation's present sources for generated frames, set by set (swapchain.c)
    D3D12_RTV_FG_SOURCES,
    D3D12_NUM_RTVS = D3D12_RTV_FG_SOURCES + D3D12_FG_SETS * D3D12_FG_MAX_GENERATED
};

/* ------------------------------------------------------------------------- */
/* device.c                                                                   */
/* ------------------------------------------------------------------------- */

bool d3d12_device_init(void);
void d3d12_device_shutdown(void);
void d3d12_wait_idle(void);
uint64_t d3d12_signal(void);
void d3d12_wait_fence(uint64_t value);
bool d3d12_fence_done(uint64_t value);
void d3d12_print_info(void);
bool d3d12_is_ultimate(void);     // DirectX 12 Ultimate: feature level 12_2 and all it promises

// HRESULT check: prints what failed and where, returns true on success
bool d3d12_check(HRESULT hr, const char *what, const char *file, int line);
#define D3D_CHECK(expr) d3d12_check((expr), #expr, __FILE__, __LINE__)

// Releases a COM object once the GPU can no longer be using it: after the frame
// being recorded (or, between frames, every frame submitted) has finished.
void d3d12_defer_release(IUnknown *object);
// ... or once the fence reaches `value`
void d3d12_defer_release_at(IUnknown *object, uint64_t value);
#define D3D12_DEFER_RELEASE(p) do { if (p) { d3d12_defer_release((IUnknown *)(p)); (p) = NULL; } } while (0)
#define D3D12_RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
void d3d12_collect_garbage(bool all);
// R_EndFrame: everything deferred while the frame was open waits for `frame_fence`
void d3d12_stamp_garbage(uint64_t frame_fence);
D3D12_CPU_DESCRIPTOR_HANDLE d3d12_rtv(UINT index);

D3D12_CPU_DESCRIPTOR_HANDLE d3d12_srv_cpu(UINT index);
D3D12_GPU_DESCRIPTOR_HANDLE d3d12_srv_gpu(UINT index);
D3D12_GPU_DESCRIPTOR_HANDLE d3d12_sampler_gpu(UINT index);

// Buffers. UPLOAD heap buffers stay mapped; DEFAULT ones start in COMMON.
ID3D12Resource *d3d12_create_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                    const char *name);

// Uploads: recorded into a command list of their own, which R_BeginFrame and
// R_EndFrame execute ahead of the frame's own list (same queue, so in order).
// The staging memory is released once the GPU is past it.
//
// A texture must be in COPY_DEST; it is left in ALL_SHADER_RESOURCE. A buffer
// may be in COMMON: buffers are promoted to COPY_DEST by the copy and decay
// back to COMMON when the list completes, so they need no barrier at all.
void d3d12_upload_texture(ID3D12Resource *dst, const void *pixels, int width, int height, int bytes_per_pixel);
// One subresource (an array layer, a mip, a 3D image's every slice - the pixels
// slice after slice), the copy alone: the texture stays in COPY_DEST, and the
// caller transitions it once every subresource is in.
bool d3d12_upload_subresource(ID3D12Resource *dst, uint32_t subresource, const void *pixels, int width, int height,
                              int bytes_per_pixel);
void d3d12_upload_buffer(ID3D12Resource *dst, uint64_t offset, const void *data, uint64_t size);
// `size` bytes of staging memory in one piece, at a D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
// offset into *buffer, for the caller to fill (from any thread) and copy from in the
// upload batch before the next d3d12_upload_flush. NULL when out of memory.
uint8_t *d3d12_upload_reserve(uint64_t size, ID3D12Resource **buffer, uint64_t *offset);
void d3d12_upload_flush(void);
// The upload batch's command list, opened if need be: for other copies that
// must run before the next frame (screenshot readback)
ID3D12GraphicsCommandList *d3d12_upload_list(void);

// D3D12SerializeVersionedRootSignature, from the d3d12.dll loaded at init
HRESULT d3d12_serialize_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob,
                                       ID3DBlob **error);

// Every resource the renderer makes is named for PIX / the debug layer.
void d3d12_set_name(ID3D12Object *object, const char *name);

// fn(ctx, i) for every i below `count`, on up to every logical processor, the calling
// thread too; back when all are done. The jobs may only call what is free-threaded:
// ID3D12Device methods, malloc - no Com_Printf, Z_Malloc, FS_* or command lists.
// `max_threads` 0 = every logical processor. Returns the threads that ran.
typedef void (*d3d12_job_fn)(void *ctx, int index);
int d3d12_parallel_for(int count, d3d12_job_fn fn, void *ctx, int max_threads);

/* ------------------------------------------------------------------------- */
/* swapchain.c                                                                */
/* ------------------------------------------------------------------------- */

bool d3d12_swapchain_create(void);
void d3d12_swapchain_destroy(void);
// recreates the buffers at the window's size if R_ModeChanged asked for it
bool d3d12_swapchain_update(void);
// blocks until DXGI wants the next frame (the frame latency waitable object)
void d3d12_swapchain_wait(void);
// records: frame target -> current backbuffer, and the backbuffer to PRESENT
void d3d12_swapchain_record_copy(ID3D12GraphicsCommandList4 *cmd);
void d3d12_present(void);
// gpu_timers.c (profiler 1, profiler_dump): vkpt's profiler rows (vkpt/profiler.h) timed
// on this queue, their averages and table vkpt's profiler.c
void d3d12_timers_frame_begin(void);                                // the slot's fence passed, frame open
void d3d12_timers_frame_end(ID3D12GraphicsCommandList4 *cmd);      // before the list closes
void d3d12_timers_get(d3d12_timers_t *out);                         // the open frame's (heap NULL: none)
void d3d12_timers_flow(int action);                                 // the Flow row, around Flow's work
void d3d12_timers_draw(void);                                       // into the 2D queue
void d3d12_timers_shutdown(void);
// reflex_nvapi.c: NVIDIA Reflex through NVAPI, with vkpt/reflex.c's cvars and policy
typedef enum {      // NV_LATENCY_MARKER_TYPE's values
    D3D12_REFLEX_SIMULATION_START = 0,
    D3D12_REFLEX_SIMULATION_END = 1,
    D3D12_REFLEX_RENDERSUBMIT_START = 2,
    D3D12_REFLEX_RENDERSUBMIT_END = 3,
    D3D12_REFLEX_PRESENT_START = 4,
    D3D12_REFLEX_PRESENT_END = 5,
    D3D12_REFLEX_INPUT_SAMPLE = 6,
} d3d12_reflex_marker_t;
void d3d12_reflex_init(void);                       // after the device and the swapchain
void d3d12_reflex_shutdown(void);
void d3d12_reflex_sleep(void);                      // R_LatencySleep
void d3d12_reflex_marker(d3d12_reflex_marker_t marker);     // the current frame's
// the present thread's: the frame it presents, which the main thread has moved past
void d3d12_reflex_marker_for_frame(d3d12_reflex_marker_t marker, uint64_t frame_id);
uint64_t d3d12_reflex_frame_id(void);               // 0 without Reflex
// frame generation's present queue (swapchain.c): its presents are out of band
void d3d12_reflex_out_of_band_present_queue(ID3D12CommandQueue *queue);
// sound_trace.c: ray traced sound (vkpt/sound_trace.c's GPU half on its own compute queue)
void d3d12_sound_init(void);                        // after the device; sets s_gpu_trace_available
void d3d12_sound_shutdown(void);
bool d3d12_sound_trace(const r_sound_trace_t *trace, const char **why);    // R_SoundTrace

// screenshots: the last composed frame, bottom-up RGB, FS_AllocTempMem'd (with HDR,
// mapped to SDR: ui_hdr_nits as white)
byte *d3d12_read_frame_target(int *width, int *height, int *rowbytes);
// IMG_ReadPixelsHDR: the scRGB frame as floats, bottom-up RGB; NULL without HDR
float *d3d12_read_frame_target_hdr(int *width, int *height);

/* FRAME GENERATION'S PRESENTATION (the comment at the top of swapchain.c). */
// moves the swapchain to the present queue (on) or back to the main queue (off), the
// GPU idle; fg_present.c's thread must be drained. False: it stays (or is back) off.
bool d3d12_fg_mode(bool on);
bool d3d12_fg_mode_active(void);
// the present sources for `generated` frames a set (made once, kept up to the most
// asked for); false when they could not be made
bool d3d12_fg_sources(int generated);
// generated frame `index` (1..)'s source in `set`: its sRGB RTV, and its state from
// COPY_SOURCE to RENDER_TARGET for drawing and back
D3D12_CPU_DESCRIPTOR_HANDLE d3d12_fg_source_rtv(int set, int index);
void d3d12_fg_source_begin(ID3D12GraphicsCommandList4 *cmd, int set, int index);
void d3d12_fg_source_end(ID3D12GraphicsCommandList4 *cmd, int set, int index);
// in place of d3d12_swapchain_record_copy: the frame target (the real frame, with
// its HUD) to `set`'s source 0
void d3d12_fg_record_real(ID3D12GraphicsCommandList4 *cmd, int set);
// main queue, before the list that writes `set`: wait for the present queue's last
// copy out of it (its presents must have been issued - FGPresent_WaitUntilPending)
void d3d12_fg_wait_set(int set);
// main queue, after the frame's list: the value the frame's presents wait for (0 = failed)
uint64_t d3d12_fg_signal_ready(void);
// the present thread's (fg_present_backend_t): host wait for a frame's work, whether
// a newer frame's is done, and one present - `set`'s source `index` copied into the
// current backbuffer on the present queue, then Present. False: not presented.
bool d3d12_fg_ready_wait(uint64_t value);
bool d3d12_fg_ready_passed(uint64_t value);
bool d3d12_fg_present(int set, int index, uint64_t ready_value);

/* ------------------------------------------------------------------------- */
/* textures.c                                                                 */
/* ------------------------------------------------------------------------- */

bool d3d12_textures_init(void);
void d3d12_textures_shutdown(void);
void d3d12_textures_begin_frame(void);
void IMG_Load_D3D12(image_t *image, byte *pic);
void IMG_Unload_D3D12(image_t *image);
byte *IMG_ReadPixels_D3D12(int *width, int *height, int *rowbytes);
// The GPU address of the byte-per-image sampler table (binding 901).
D3D12_GPU_VIRTUAL_ADDRESS d3d12_sampler_index_buffer(void);
// Where the bindless table a frame of parity `odd` (d3d.frame_counter & 1) reads
// starts in the shader-visible heap: the textures sets' and the 2D layer's base.
UINT d3d12_bindless_base(int odd);
ID3D12Resource *d3d12_sampler_index_resource(void);
// TEX_BLUE_NOISE's 2D array, loaded on first call (in the upload batch); NULL
// without the files
ID3D12Resource *d3d12_blue_noise(void);

/* ------------------------------------------------------------------------- */
/* shaders.c                                                                  */
/* ------------------------------------------------------------------------- */

// baseq2/shader_d3d12/<name>.dxil, made by the build from the same GLSL as the
// Vulkan .spv of that name. FS_FreeFile the result.
void *d3d12_load_shader(const char *name, size_t *size);

/* ------------------------------------------------------------------------- */
/* draw.c - the 2D layer                                                      */
/* ------------------------------------------------------------------------- */

bool d3d12_draw_init(void);
void d3d12_draw_shutdown(void);
void d3d12_draw_submit(ID3D12GraphicsCommandList4 *cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, int width, int height);

/* ------------------------------------------------------------------------- */
/* world.c - the 3D view (phase 2)                                            */
/* ------------------------------------------------------------------------- */

// One static BLAS to build at map load: a model_geometry_t's geometries, each a
// run of triangles in the world buffer's positions.
typedef struct {
    const uint32_t  *prim_offsets;      // in triangles from the start of the positions
    const uint32_t  *prim_counts;
    uint32_t        num_geometries;
    uint64_t        address;            // out: the BLAS, 0 if none was built
} d3d12_world_blas_t;

typedef struct {
    // vkpt's buf_world minus its BLAS memory: VboPrimitive[], then the positions
    const void          *data;
    uint64_t            size;
    uint64_t            primitives_size;    // what primitive_buffers[VERTEX_BUFFER_WORLD] covers
    uint64_t            positions_offset;   // float3 x 3 per primitive
    d3d12_world_blas_t  *blases;
    int                 num_blases;
    // light_stats_bufers[]: 2 uints per light poly, per orientation (6), per cluster
    uint64_t            light_stats_count;
} d3d12_world_map_t;

// What frontend.c hands the frame, from vkpt_prepare_frame
// transparency.c's streams (vkpt's VKPT_TR_*): the frame's particles, beams
// and sprites, each copied to the start of its own buffer
enum {
    D3D12_TR_VERTICES,                  // float3: the particles' quads, then the sprites'
    D3D12_TR_BEAM_AABBS,                // D3D12_RAYTRACING_AABB
    D3D12_TR_PARTICLE_COLORS,           // RGBA32F texel buffer
    D3D12_TR_BEAM_COLORS,               // RGBA32F texel buffer
    D3D12_TR_SPRITE_INFOS,              // RGBA32UI texel buffer
    D3D12_TR_BEAM_INTERSECT,            // RGBA32UI texel buffer
    D3D12_TR_NUM_STREAMS,
    D3D12_TR_INDICES = D3D12_TR_NUM_STREAMS,    // uint16 quads, filled once
};

// blood.c's frame (vkpt_blood_frame_t): the section of the instanced buffers,
// and the regions of blood.c's shadow copy that have to reach it
// (d3d12_blood_region_t: passes.h)
typedef struct {
    uint32_t            prim_offset, prim_count;    // prim_count 0: no blood
    bool                zero_section;       // the device's copy is void: zero it, then copy
    uint32_t            num_regions;
    const d3d12_blood_region_t *regions;    // ascending, disjoint
    const void          *prims, *positions; // VboPrimitive[], 9 floats a primitive
    uint32_t            capacity;           // the shadow copy's primitives
} d3d12_world_blood_t;

// vkpt_transparency_frame's layout of this frame's CPU buffer
typedef struct {
    const void          *data;
    uint64_t            upload_size;
    uint64_t            offset[D3D12_TR_NUM_STREAMS], size[D3D12_TR_NUM_STREAMS];
    uint32_t            particle_num, beam_num, sprite_num;
    uint64_t            sprite_vertex_offset;   // in the vertex buffer, bytes
} d3d12_world_transparency_t;

// shadow_map.c's instance list (vkpt_shadow_map_instance_t): a static model or brush
// entity in the sun's shadow map - its positions in the world buffer or a model slot's
// VBO, and its matrix times the view-projection
typedef struct {
    float               mvp[16];            // column-major
    int                 model;              // -1: the world buffer, else a model slot
    uint64_t            positions_offset;   // bytes into that buffer
    uint32_t            prim_count;
} d3d12_shadow_instance_t;

// NVIDIA Flow's frame (vkpt/flow.c's vkpt_flow_dispatch_t, and path_tracer.c's choices)
typedef struct {
    int                 set;                // the frame's parity: the buffers' slices
    uint32_t            occupancy_blocks;   // the passes' groups (d3d12_record_flow), 0 = none
    uint32_t            light_blocks;
    uint32_t            env_groups[3];
    bool                split;              // the march as its own pass (pt_flow_split_pass)
    bool                reflect;            // the reflect/refract bounces with the march
} d3d12_world_flow_t;

typedef struct {
    // RDF_NOWORLDMODEL: the player setup menu's model view - no world instances, no sky,
    // no fog, and maybe no map loaded at all (the main menu). Drawn over the whole
    // screen as vkpt does: the menu's rectangle is in the projection (prepare_ubo)
    bool                no_world;
    const void          *ubo;               // QVKUniformBuffer_t
    const void          *instance_buffer;   // InstanceBuffer
    const D3D12_RAYTRACING_INSTANCE_DESC *instances;   // vkpt's TLAS instance list
    uint32_t            num_instances;
    d3d12_extents_t     extents;            // the G-buffer images' sizes
    uint32_t            render_width, render_height;
    uint32_t            field_width;        // one checkerboard field (vkpt_pt_field_width)
    uint32_t            slice_width;        // both fields (qvk.gpu_slice_width, one GPU)
    uint32_t            taa_output_width, taa_output_height;
    uint32_t            num_light_regions;  // what to copy of this frame's light staging
    uint64_t            light_offsets[2], light_sizes[2];
    // vkpt_prepare_frame's reference mode and the switches vkpt reads from cvars
    bool                enable_denoiser;    // A-SVGF; else plain compositing
    float               num_bounce_rays;    // 0, 0.5 (half-height), 1..8
    bool                caustics;           // pt_caustics: direct lighting's variant
    bool                sharc_active;       // vkpt_frame_prep_t's: the radiance cache runs
    bool                sharc_clear;        // ...and starts from empty
    bool                bloom;              // vkpt_bloom_will_run
    float               bloom_argument_scale, bloom_normalization_scale;    // vkpt_bloom_blur_params
    int                 bloom_num_samples;
    int                 bloom_debug;        // bloom_debug's view (1..3), 0 none
    // motion blur and chromatic aberration, as passes.h's d3d12_passes_frame_t has them
    bool                motion_blur, motion_blur_to_bloom;
    uint32_t            motion_blur_push[D3D12_MOTION_BLUR_PUSH_WORDS];
    bool                chromatic_aberration;
    uint32_t            chromatic_aberration_push[D3D12_CHROMATIC_ABERRATION_PUSH_WORDS];
    // film grain and the CRT (vkpt_final_blit_effects_params at the window's size):
    // every presented frame's blit draws through them, with its own source and seed
    bool                blit_effects;
    uint32_t            blit_effects_push[D3D12_BLIT_PUSH_WORDS];
    // DLSSEnabled: frontend.c's d3d12_frontend_dlss_record upscales TAA_OUTPUT into
    // DLSS_OUTPUT, which bloom, tone mapping and the screen then use at the output size
    bool                dlss;
    // photo mode (vkpt_fg_bypassed): NGX is skipped, and the accumulated TAA_OUTPUT is
    // copied into DLSS_OUTPUT in its place (vkpt_blit_accumulated_to_dlss_output)
    bool                dlss_bypass;
    // DLSS 5 Neural Rendering (vkpt/DLSSNR.c) on DLSS_OUTPUT after tone mapping, not in
    // photo mode: frontend.c's d3d12_frontend_dlss5_record makes the feature and
    // evaluates it, between dlss5_convert.comp's encode and resolve ([0], [1] their push
    // constants)
    bool                dlss5;
    uint32_t            dlss5_push[2][D3D12_DLSS5_PUSH_WORDS];
    bool                taa_folded;         // vkpt_taa_folded
    float               frame_time;         // seconds, for the eye adaptation
    // the physical sky: render it before the primary rays (vkpt_physical_sky_needs_update),
    // as physical_sky_space.comp, through terrain_shadowmap_viewproj
    bool                sky_update;
    bool                sky_space;
    float               sky_viewproj[16];
    // the models: instance_geometry.comp's groups (vkpt's upload_info.num_instances);
    // the world's texture animation steps this frame (animate_materials.comp); how much
    // of the IQM staging to copy (vkpt_iqm_upload_size); of the instance list, how many
    // are geometry (the rest are the effects TLAS's); pt_tlas_fast_trace
    uint32_t            num_model_instances;
    bool                animate_world;
    uint64_t            iqm_upload_size;
    uint32_t            num_geometry_instances;
    bool                tlas_fast_trace;
    // THE PARTITIONED TLAS (vkpt/ptlas.h, accel.h): pt_ptlas; of the list, the
    // static world's instances (its head: the global partition) and where the
    // dynamic BLASes' start (rebuilt every frame: rewritten every frame)
    bool                ptlas;
    uint32_t            num_world_instances;
    uint32_t            first_dynamic_instance;
    // the particles, beams and sprites (vkpt's update_transparency)
    d3d12_world_transparency_t tr;
    // the blood droplets (vkpt_blood_update)
    d3d12_world_blood_t blood;
    // the fog (god_rays.c), vkpt_frame_prep_t's switches: all of it (god_rays_enabled, and
    // d3d12_world_fog_ready), the march (not god_rays_march_discarded), a ray per pixel
    // (photo mode's fog_accum_march), the froxel grid (run_froxel) and its light selection
    // (pt_fog_restir), a batch of the sky exposure bake (count 0: none)
    bool                god_rays;
    bool                god_rays_march;
    bool                fog_accum_march;
    bool                froxel;
    bool                froxel_restir;
    uint32_t            sky_exposure_first, sky_exposure_count;
    // the sun's shadow map (vkpt_shadow_map_render's arguments): its view-projection, in
    // vertices the world's opaque run, the instanced buffer's opaque models and the water
    // (the world's transparent run, the second layer), and the static instances
    float               shadowmap_viewproj[16];
    uint32_t            smap_opaque_first, smap_opaque_count;
    uint32_t            smap_dynamic_count;
    uint32_t            smap_water_first, smap_water_count;
    const d3d12_shadow_instance_t *smap_instances;
    uint32_t            smap_num_instances;
    // TILE COMPACTION: vkpt_pt_tile_header's header for this view (PT_TILE_HDR_WORDS
    // words; world.c sizes the lists by its layout), NULL = the reflect/refract bounces
    // cover the whole grid
    const uint32_t      *tile_header;
    // NVIDIA Flow's passes and variants (all zero: none, the march compiled out)
    d3d12_world_flow_t  flow;
} d3d12_world_frame_t;

// Whether a frame's blood upload was dropped since the last call (no memory
// for its staging) - frontend.c then calls vkpt_blood_invalidate, so the next
// frame sends the section afresh. Clears the flag.
bool d3d12_world_take_blood_lost(void);
// the last frame's blood upload memcpy, in microseconds (pt_blood_stats)
uint64_t d3d12_world_blood_copy_usec(void);

// Map load, with blood on (vkpt_prep_backend->reserve_blood_staging): every frame
// slot's blood staging made now at blood.c's capacity (primitives), so the first
// droplet allocates nothing. Waits for the GPU only when a slot must be remade.
void d3d12_world_reserve_blood_staging(uint32_t capacity);

// One model's VBO, vkpt_model_vbo_layout's: VboPrimitive[], then for a static
// model (one frame, no skeleton) its positions and the geometries of its BLASes
typedef struct {
    const void          *data;
    uint64_t            size;
    uint64_t            primitives_size;    // what primitive_buffers[VERTEX_BUFFER_FIRST_MODEL + i] covers
    uint64_t            positions_offset;   // a static model's
    d3d12_world_blas_t  blases[3];          // a static model's opaque, transparent, masked; addresses out
    int                 num_blases;         // 0: an animated model, instanced through instance_geometry
} d3d12_world_model_t;

// vkpt_pt_create_all_dynamic's BLASes, in vkpt_dyn_blas_t's order: the first
// seven are runs of the instanced positions (the model sections, blood and the
// explosions), then the particles' and sprites' quads and the beams' boxes out
// of the transparency buffers
enum {
    D3D12_DYN_OPAQUE,
    D3D12_DYN_TRANSPARENT,
    D3D12_DYN_MASKED,
    D3D12_DYN_VIEWER_WEAPON,
    D3D12_DYN_VIEWER_MODELS,
    D3D12_DYN_BLOOD,
    D3D12_DYN_EXPLOSIONS,
    D3D12_NUM_DYN_RUNS,
    D3D12_DYN_PARTICLES = D3D12_NUM_DYN_RUNS,
    D3D12_DYN_BEAMS,
    D3D12_DYN_SPRITES,
    D3D12_NUM_DYN
};

enum {
    D3D12_WORLD_STAGING_LIGHTS,         // LightBuffer, written by the frame's CPU half
    D3D12_WORLD_STAGING_IQM,            // IqmMatrixBuffer
    D3D12_WORLD_STAGING_READBACK,       // ReadbackBuffer, as this slot's last frame left it
    D3D12_NUM_WORLD_STAGING
};

// vkpt/DLSS.c (declared in DLSS.h, which pulls in Vulkan): pt_dlss_debug's VKPT_IMG_*
// to show raw, or -1. world.c shows it in place of d3d12_show's image.
int DLSSDebugImage(int *pair);

bool d3d12_world_init(void);
void d3d12_world_shutdown(void);
bool d3d12_world_ready(void);
// map load: waits for the GPU, replaces the world buffer and static BLASes,
// fills in each blases[i].address
bool d3d12_world_load(d3d12_world_map_t *map);
void d3d12_world_unload(void);
bool d3d12_world_loaded(void);
// this frame slot's staging, persistently mapped
void *d3d12_world_staging(int which);
// records the 3D view into the frame target (in RENDER_TARGET); false = it did
// not (no map, no G-buffer) - nor the sky the frame asked for
bool d3d12_world_render(ID3D12GraphicsCommandList4 *cmd, const d3d12_world_frame_t *frame);
// a G-buffer image (VKPT_IMG_*), simultaneous-access, in COMMON between lists; NULL
// before the first frame made them
ID3D12Resource *d3d12_world_image(int image);
// d3d12_vram's figures: the G-buffer's bytes and size, the froxel volumes' bytes (0 = not made)
void d3d12_world_memory(uint64_t *gbuffer, uint32_t *width, uint32_t *height, uint64_t *froxel);
// d3d12_vram's: every image's texture, its count in *count
uint64_t d3d12_textures_memory(int *count);
// the even (0) or odd (1) textures set, NULL without the world (flow.c writes its bindings)
const d3d12_set_t *d3d12_world_textures_set(int odd);
// a volume zeroed in the upload batch ahead of the next frame (copies out of the zero
// buffer; it must be able to leave COMMON for COPY_DEST and back)
bool d3d12_world_zero_volume(ID3D12Resource *volume);

// flow.c: NVIDIA Flow's buffers, dummies and probe grid - vkpt/flow.c's Vulkan half,
// which frontend.c's d3d12_flow_backend hands it. Made with the world; the GPU idle
// for the destroys.
bool d3d12_flow_create(size_t params_stride, size_t light_sums_stride, size_t occupancy_stride,
                       uint8_t **params_mapped, uint8_t **light_sums_mapped);
void d3d12_flow_destroy(void);
bool d3d12_flow_create_env(void);                   // the probe grid (pt_flow_env)
void d3d12_flow_destroy_env(void);
// set's textures-set Flow bindings: Flow's density and block table (format 0 = the
// texture's own), or the dummies for NULL
void d3d12_flow_bind(int set, ID3D12Resource *density, DXGI_FORMAT density_format, ID3D12Resource *table);
// passes.c's d3d12_record_flow for this frame; returns the probe pass's set for frame
// slot `slot`, `tlas` written into it - NULL when there are no probes this frame
const d3d12_set_t *d3d12_flow_frame(const d3d12_world_flow_t *flow, int slot, D3D12_GPU_VIRTUAL_ADDRESS tlas,
                                    d3d12_flow_passes_t *out);

// The fog's resources: the sun's shadow map, the bake's mask, the god rays sets (made
// with the world; without them there is no fog)
bool d3d12_world_fog_ready(void);
// The froxel grid's volumes (god_rays.c's create_froxel_volumes), the first frame the
// grid is wanted - vkpt_prep_backend's create_froxel_volumes: zeroed ahead of that frame,
// the god rays sets rewritten (after waiting for the GPU). A failure is not retried
// before the next d3d12_world_init.
bool d3d12_world_froxel_create(void);
// FRAME GENERATION, after d3d12_world_render drew a frame (nothing otherwise):
// dlssg_hdr.comp `mode` over `target` (0 DLSS_OUTPUT, 1..5 DLSS_FG_OUTPUT..5), and
// generated frame `index` blitted to the screen-sized `rtv` as the frame's was
void d3d12_world_dlssg_convert(ID3D12GraphicsCommandList4 *cmd, uint32_t mode, uint32_t target);
void d3d12_world_blit_generated(ID3D12GraphicsCommandList4 *cmd, unsigned int index, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
// pt_fog_sky_exposure's solid mask for the new map (vkpt_sky_exposure_fit_map's)
void d3d12_world_sky_exposure_mask(const uint32_t *bits, uint32_t words);

// The physical sky's tables (world.c, sky.c). The preset's: its AtmosphereParameters
// (the sky set's constants) and env/{transmittance,inscatter,irradiance}_<planet>.dds.
// Waits for the GPU: the textures sets are rewritten in place.
void d3d12_world_sky_load_preset(const void *atmosphere, size_t atmosphere_size, const char *planet);
// the terrain shadow map's grid (SkyBuildTerrainGrid's), once
void d3d12_world_sky_set_terrain(const float *vertices, uint32_t num_vertices, const uint32_t *indices,
                                 uint32_t num_indices);
// R_SetSky's cube, TEX_ENVMAP: six w x h RGBA8 faces back to back. Waits for the GPU.
void d3d12_world_upload_envmap(int width, int height, const byte *data);
// cpu_bench's copy of the physical sky's cube (the prep backend's read_sky_envmap): six
// w x h RGBA16F faces back to back, Z_Malloc'd; between frames, waits for the GPU
uint16_t *d3d12_world_read_sky_envmap(int *width, int *height);
// the FG debug tools' read (vkpt_fg_read_t): RGBA16F texels of a VKPT_IMG_* image's
// w x h region at (x, y), packed into out; through the upload list, waits for the GPU
bool d3d12_world_read_image(int image, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t *out);

// The models' VBOs (vkpt's model_vertex_data, slot `index`): the buffer and a
// static model's BLASes recorded into the upload list, the BLAS addresses
// filled in. A free (or an upload over a slot in use) lets the old ones go.
// The descriptors change at the commit, which flushes the uploads and waits
// for the GPU - as vkpt_vertex_buffer_upload_models waits for its queue.
bool d3d12_world_model_upload(int index, d3d12_world_model_t *model);
void d3d12_world_model_free(int index);
void d3d12_world_models_commit(void);

// The instanced buffers' size in primitives (pt_primbuf): a change waits for
// the GPU and makes them afresh
uint32_t d3d12_world_primbuf_size(void);
void d3d12_world_set_primbuf_size(uint32_t prims);

// This frame's dynamic BLASes - the runs of the instanced buffers (first
// primitive and count of each) and the transparency geometry `tr` describes -
// laid out in this frame slot's memory: their addresses, 0 = none, for the
// instance list before d3d12_world_render builds them. pt_blas_fast_trace's
// build flags. blood_reuse: blood.c vouches the blood section is what this slot's
// blood BLAS was built from (vkpt_blood_blas_needs_build false), so it may stand;
// blood_built: it was (re)built this frame (for vkpt_blood_blas_built).
void d3d12_world_plan_dynamic(const uint32_t first[D3D12_NUM_DYN_RUNS], const uint32_t count[D3D12_NUM_DYN_RUNS],
                              const d3d12_world_transparency_t *tr, bool fast_trace, bool blood_reuse,
                              uint64_t address[D3D12_NUM_DYN], bool *blood_built);

// The transparency buffers (transparency.c's create_buffers): each stream's
// capacity in bytes, D3D12_TR_INDICES' too, the CPU frame's (for the staging
// slices), and the index pattern, uploaded once. Without them the effects
// TLAS stays empty. Waits for the GPU.
bool d3d12_world_transparency_init(const uint64_t capacity[D3D12_TR_NUM_STREAMS + 1], uint64_t frame_capacity,
                                   const uint16_t *indices);

/* ------------------------------------------------------------------------- */
/* frontend.c - what is borrowed from vkpt (the only file that includes vkpt.h)*/
/* ------------------------------------------------------------------------- */

void d3d12_frontend_init(void);
void d3d12_frontend_shutdown(void);
void d3d12_frontend_mesh_shutdown(void);  // the CPU trace cache, after the models
void d3d12_frontend_begin_registration(const char *map);
void d3d12_frontend_end_registration(void);
// R_BeginFrame: vkpt_vertex_buffer_upload_models' walk - the models registered
// since are uploaded, the freed ones let go
void d3d12_frontend_upload_models(void);
// vkpt_prepare_frame for the 3D view; false = nothing to draw (no world yet)
bool d3d12_frontend_prepare_frame(refdef_t *fd, int waterLevel, d3d12_world_frame_t *frame);
// once the frame is recorded (`drawn`: d3d12_world_render's answer):
// vkpt_finish_frame (next frame's history flags), the sky marked rendered
void d3d12_frontend_finish_frame(bool drawn);
// cpu_bench's screen, R_RenderFrame's first test: true = the frame is the CPU path
// tracer's picture (a 2D pic), and no world is rendered
bool d3d12_frontend_cpu_bench_draw(refdef_t *fd);
// vid_present_stats: whether it is on, and R_BeginFrame's two waits (microseconds)
bool d3d12_frontend_present_stats_on(void);
// R_EndFrame's debug: tm_debug's line over the HUD; pt_dlss_fg_compare / _dump over the
// frame's finished images (after its list is executed); pt_dlss_fg_show
void d3d12_frontend_draw_debug(void);
void d3d12_frontend_fg_debug(unsigned int generated);
bool d3d12_frontend_fg_show_generated(void);
void d3d12_frontend_present_stats(uint64_t fence_us, uint64_t present_wait_us);
// passes.c's upscale callback (d3d12_passes_t), while d3d12_world_render records a
// frame with `dlss`: DLSSApply through NGX's D3D12 entry points (ngx.c) - the
// feature made or remade as this frame's sizes and settings say, then evaluated
void d3d12_frontend_dlss_record(void *ctx, ID3D12GraphicsCommandList4 *cmd);
// passes.c's DLSS 5 callback (d3d12_passes_t's `neural`), for a frame with `dlss5`:
// stage 0 DLSS5Prepare (the feature made or remade; false: not this frame), stage 1
// the evaluation through ngx.c (false: it failed, and DLSS 5 is off until toggled)
bool d3d12_frontend_dlss5_record(void *ctx, ID3D12GraphicsCommandList4 *cmd, int stage);
/* FRAME GENERATION (pt_dlss_fg), DLSSGApply's D3D12 half. */
// R_BeginFrame, before anything else: the swapchain on the present queue and
// fg_present.c's thread running while frame generation is wanted, back otherwise
void d3d12_frontend_fg_begin_frame(void);
// after d3d12_world_render drew a frame with DLSS: the feature made as this frame
// needs, the colour encoded, the generated frames evaluated, all decoded again.
// Returns how many generated frames there are (0: none this frame).
unsigned int d3d12_frontend_dlssg_record(ID3D12GraphicsCommandList4 *cmd);
// R_EndFrame with the swapchain on the present queue: before the frame's list is
// executed (the present sources of `set` free to write), and after it (its
// presents - `generated` generated frames, then the real one - to the thread)
void d3d12_frontend_fg_before_submit(int set);
void d3d12_frontend_fg_present(int set, unsigned int generated);
// every queued present issued (before the swapchain's buffers change)
void d3d12_frontend_fg_drain(void);
// Reflex_DetectVRR for the window's monitor (vkpt asks at every swapchain creation)
void d3d12_frontend_detect_vrr(void);
// stretch_pic.frag's UBO (vkpt_draw_ui_hdr_params): the UI's nits on an HDR screen,
// its saturation, the video frames' curve
void d3d12_frontend_ui_hdr_params(float *ui_hdr_nits, float *saturation_scale, float *video_gamma);
// ...and its ui_hdr_gamma (vkpt_draw_ui_hdr_gamma): tm_hdr_sdr_gamma, 0 = off
float d3d12_frontend_ui_hdr_gamma(void);
// the swapchain's HDR10 metadata (vkpt_hdr_metadata), in nits
void d3d12_frontend_hdr_metadata(float *max_mastering, float *min_mastering, float *max_cll, float *max_fall);
// after d3d12_world_init: the images the UBO names (the water normal map), the
// terrain grid, and the sky's preset loaded afresh
void d3d12_frontend_world_init(void);
// right after the device is made: NGX started on a thread of its own (pt_dlss on, an
// NVIDIA adapter), so the first map does not wait for it
void d3d12_frontend_ngx_prestart(void);
// com_vram_usage (the map load's VRAM line) while the device is up
void d3d12_vram_hook(bool on);
// d3d12_vram: the process's video memory against its budget, and the parts that take it
void d3d12_print_vram(void);
// the 2D queue vkpt's R_Draw*_RTX functions fill
const void *d3d12_frontend_stretch_pics(int *count, size_t *record_size, int *max_count);
void d3d12_frontend_clear_stretch_pics(void);
// which D3D12_GLOBAL_SAMPLER_* an image is read through (vkpt_image_sampler_kind)
int d3d12_frontend_sampler_kind(const image_t *image, int pt_nearest, int bilerp_chars, int bilerp_pics);
void d3d12_frontend_register_functions(void);

#endif // REFRESH_D3D12PT_H
