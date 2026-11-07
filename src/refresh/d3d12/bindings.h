/*
THE BINDING LAYER: vkpt's descriptor sets, on D3D12.

The shaders were written against Vulkan descriptor sets and are translated, not
rewritten, so on D3D12 they still name (set, binding) - spv2dxil turns each into
register 0 of space 1000 * (set + 1) + binding. This file holds the other half
of that contract:

  * SET LAYOUTS. Each Vulkan set layout vkpt makes (the global textures and
    images, the UBO, the vertex buffers, the path tracer's TLAS set, ...) is
    described here once, from the SAME macros the shaders compile against
    (global_textures.h's LIST_IMAGES, vertex_buffer.h's *_BINDING_IDX), so a
    binding added to a shader header reaches this side without a second list.

  * A SET INSTANCE is a block of descriptors in the shader-visible heaps - one
    for resources, one for samplers - laid out binding by binding at the
    offsets the layout computed. Writing a descriptor is "binding b, element
    i of this block", as vkUpdateDescriptorSets was.

  * PIPELINE LAYOUTS are the handful of set lists vkpt's passes use (path
    tracer, "standard" post passes, ...). Each becomes one root signature:
    per set a resource table and a sampler table, an own table for every
    unbounded array (a table can end in at most one), 32 root constants for
    the push constants (Vulkan's guaranteed 128 bytes) and 3 for
    gl_NumWorkGroups. A pass's PSO is made against its layout's root
    signature, and D3D12 refuses one whose shader names a register the
    signature does not cover - which is what tests/d3d12_pso_test.c leans on
    to check every translated shader against these tables.

No engine headers here: the headless tests compile bindings.c on its own.
*/

#ifndef REFRESH_D3D12_BINDINGS_H
#define REFRESH_D3D12_BINDINGS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d12.h>

/* ------------------------------------------------------------------------- */
/* THE BINDING RULE - must match spv2dxil/spv2dxil.cpp                        */
/* ------------------------------------------------------------------------- */

/* Every Vulkan (set, binding) is register 0 of a register space of its own,
   so arrays never overlap the next binding. Push constants are root constants
   at b0 space0; gl_NumWorkGroups (when a shader reads it) root constants at
   b0 space1. */
#define D3D12_SPACE(set, binding)       (1000u * ((unsigned)(set) + 1u) + (unsigned)(binding))
#define D3D12_SPACE_PUSH_CONSTANTS      0u
#define D3D12_SPACE_NUM_WORKGROUPS      1u

/* The path tracer's own set (path_tracer.c, whose RAY_GEN_*_BINDING_IDX are
   private to it - keep in step). */
#define RT_BINDING_TLAS                 0
#define RT_BINDING_PARTICLE_COLOR       1
#define RT_BINDING_BEAM_COLOR           2
#define RT_BINDING_SPRITE_INFO          3
#define RT_BINDING_BEAM_INTERSECT       4

/* The path tracer's tile lists: a buffer device address on Vulkan, a storage
   buffer in this set on D3D12 (shader/path_tracer.h, PT_TILE_DESC_SET_D3D12). */
#define D3D12_PT_TILE_DESC_SET          15

/* The bindless image array, split for D3D12 (shader/global_textures.h): images
   at binding 0 of the global textures set, these four samplers at binding 900,
   and a byte per image naming its sampler at binding 901. The order is the
   order of vkpt_image_sampler_kind's answers. */
#define D3D12_BINDING_GLOBAL_SAMPLERS       900
#define D3D12_BINDING_GLOBAL_SAMPLER_INDEX  901
enum {
    D3D12_GLOBAL_SAMPLER_DEFAULT,               // trilinear, 16x aniso, repeat
    D3D12_GLOBAL_SAMPLER_NEAREST,               // point, mip 0 only
    D3D12_GLOBAL_SAMPLER_NEAREST_MIPMAP_ANISO,  // point mag, linear mips, aniso
    D3D12_GLOBAL_SAMPLER_LINEAR_CLAMP,          // bilinear, clamp
    D3D12_NUM_GLOBAL_SAMPLERS
};

/* ------------------------------------------------------------------------- */
/* SET LAYOUTS                                                                */
/* ------------------------------------------------------------------------- */

typedef enum {
    D3D12_BIND_CBV,                 // uniform buffer           -> cbuffer
    D3D12_BIND_UAV_BUFFER,          // storage buffer           -> RWByteAddressBuffer (always a UAV)
    D3D12_BIND_UAV_IMAGE,           // storage image            -> RWTexture*
    D3D12_BIND_SRV_IMAGE,           // combined image sampler   -> Texture* + SamplerState, same space
    D3D12_BIND_SRV_TEXEL_BUFFER,    // uniform texel buffer     -> Buffer<>
    D3D12_BIND_SRV_AS,              // acceleration structure   -> RaytracingAccelerationStructure
    D3D12_BIND_SAMPLERS,            // separate samplers        -> SamplerState
    D3D12_BIND_SRV_IMAGE_SEPARATE,  // texture2D[] (no sampler) -> Texture2D
    D3D12_NUM_BIND_KINDS
} d3d12_bind_kind_t;

typedef struct {
    uint16_t    binding;
    uint8_t     kind;           // d3d12_bind_kind_t
    bool        unbounded;      // a runtime-sized array: own root table, `count` slots reserved
    uint32_t    count;          // array size (slots reserved when unbounded)
    // computed by d3d12_bindings_build_layouts:
    uint32_t    offset;         // in the instance's resource block; SAMPLERS: in its sampler block
    uint32_t    sampler_offset; // SRV_IMAGE: its sampler's slot in the sampler block
} d3d12_binding_t;

#define D3D12_MAX_SET_BINDINGS  320

/* inc/shared/shared.h's MAX_MODELS, which sizes the vertex buffer set's
   primitive_buffers[] (frontend.c checks the two agree). */
#define D3D12_MAX_MODELS        512

typedef struct {
    const char      *name;
    int             num_bindings;
    d3d12_binding_t bindings[D3D12_MAX_SET_BINDINGS];
    uint32_t        num_resources;  // CBV/SRV/UAV descriptors in one instance
    uint32_t        num_samplers;   // sampler descriptors in one instance
} d3d12_set_layout_t;

typedef enum {
    D3D12_SET_TEXTURES,         // global textures + every G-buffer image (textures.c)
    D3D12_SET_UBO,              // global UBO + instance buffer (uniform_buffer.c)
    D3D12_SET_VERTEX_BUFFER,    // primitives, positions, lights, readback, SHaRC... (vertex_buffer.c)
    D3D12_SET_RT,               // the path tracer's TLASes and transparency buffers (path_tracer.c)
    D3D12_SET_PT_TILES,         // the path tracer's tile lists (D3D12 only, set 15)
    D3D12_SET_GOD_RAYS,         // god rays / froxel fog (god_rays.c)
    D3D12_SET_FLOW_ENV,         // NVIDIA Flow's world probes (flow.c)
    D3D12_SET_SOUND,            // ray traced sound (sound_trace.c)
    D3D12_SET_NORMALIZE,        // normal map normalisation at load (textures.c)
    D3D12_SET_SKY,              // the precomputed sky's UBO (precomputed_sky.c)
    D3D12_NUM_SET_LAYOUTS
} d3d12_set_layout_id_t;

extern d3d12_set_layout_t d3d12_set_layouts[D3D12_NUM_SET_LAYOUTS];

/* Where the binding layer's error messages go (stderr until the renderer
   points it at the console). */
extern void (*d3d12_bindings_print)(const char *text);

const d3d12_binding_t *d3d12_set_find_binding(const d3d12_set_layout_t *layout, unsigned binding);

/* ------------------------------------------------------------------------- */
/* PIPELINE LAYOUTS                                                           */
/* ------------------------------------------------------------------------- */

#define D3D12_MAX_PIPELINE_SETS     5
#define D3D12_PUSH_CONSTANT_DWORDS  32      // Vulkan's guaranteed maxPushConstantsSize, 128 bytes
#define D3D12_MAX_UNBOUNDED_PER_SET 2

typedef enum {
    D3D12_PL_PATH_TRACER,       // rt, ubo, textures, vertex buffer (+ tiles at 15)
    D3D12_PL_STANDARD,          // ubo, textures, vertex buffer
    D3D12_PL_UBO_TEXTURES,      // ubo, textures
    D3D12_PL_UBO_VERTEX_BUFFER, // ubo, vertex buffer
    D3D12_PL_GOD_RAYS,          // god rays, vertex buffer, ubo, textures
    D3D12_PL_FLOW_ENV,          // flow env, vertex buffer, ubo, textures
    D3D12_PL_SOUND,             // sound
    D3D12_PL_NORMALIZE,         // normalize
    D3D12_PL_PHYSICAL_SKY,      // ubo, textures, vertex buffer, sky
    D3D12_PL_SHADOW_MAP,        // no sets, a vertex buffer (shadow_map.c's pipeline_layout_smap)
    D3D12_NUM_PIPELINE_LAYOUTS
} d3d12_pipeline_layout_id_t;

typedef struct {
    const char          *name;
    bool                input_assembler;    // the root signature allows an input layout
    int                 num_sets;
    int                 set_layout[D3D12_MAX_PIPELINE_SETS];   // d3d12_set_layout_id_t
    int                 set_index[D3D12_MAX_PIPELINE_SETS];    // the Vulkan set number it sits at
    // computed by d3d12_bindings_create_root_signatures; -1 = absent
    ID3D12RootSignature *root_signature;
    int8_t              param_resources[D3D12_MAX_PIPELINE_SETS];
    int8_t              param_samplers[D3D12_MAX_PIPELINE_SETS];
    int8_t              param_unbounded[D3D12_MAX_PIPELINE_SETS][D3D12_MAX_UNBOUNDED_PER_SET];
    int8_t              param_push_constants;
    int8_t              param_num_workgroups;
} d3d12_pipeline_layout_t;

extern d3d12_pipeline_layout_t d3d12_pipeline_layouts[D3D12_NUM_PIPELINE_LAYOUTS];

/* Fill d3d12_set_layouts and the pipeline layouts' set lists. CPU only, and
   idempotent: call it before anything reads the tables. */
void d3d12_bindings_build_layouts(void);

/* One root signature per pipeline layout. On failure `error` says which layout
   and why (the serializer's own message when it has one). */
HRESULT d3d12_bindings_create_root_signatures(ID3D12Device *device,
                                              PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE serialize,
                                              char *error, size_t error_size);
void d3d12_bindings_destroy_root_signatures(void);

/* ------------------------------------------------------------------------- */
/* SET INSTANCES - vkAllocateDescriptorSets / vkUpdateDescriptorSets          */
/* ------------------------------------------------------------------------- */

/* Where set instances live: a region of each shader-visible heap. The renderer
   (device.c) or a test fills this in once the heaps exist; instances are
   bump-allocated from it and live as long as the heaps (a new device fills it
   in afresh). */
typedef struct {
    ID3D12Device                *device;
    D3D12_CPU_DESCRIPTOR_HANDLE resource_cpu, sampler_cpu;     // heap starts
    D3D12_GPU_DESCRIPTOR_HANDLE resource_gpu, sampler_gpu;
    UINT                        resource_increment, sampler_increment;
    uint32_t                    resource_next, resource_end;   // the set region, heap indices
    uint32_t                    sampler_next, sampler_end;
} d3d12_descriptor_heaps_t;

extern d3d12_descriptor_heaps_t d3d12_heaps;

typedef struct {
    int         layout;                 // d3d12_set_layout_id_t; -1 = not allocated
    uint32_t    resource_base;          // heap index of the instance's resource block
    uint32_t    sampler_base;           // heap index of its sampler block
    // heap index for each unbounded array with no slots of its own (the
    // global textures' bindless block, ...), in binding order
    uint32_t    external[D3D12_MAX_UNBOUNDED_PER_SET];
} d3d12_set_t;

/* A new instance of a set layout, every descriptor a null one of its kind (so
   a binding nothing wrote reads zeros instead of hanging the GPU). */
bool d3d12_set_alloc(d3d12_set_t *set, d3d12_set_layout_id_t layout, const char *what);

void d3d12_set_write_cbv(const d3d12_set_t *set, unsigned binding, unsigned element,
                         D3D12_GPU_VIRTUAL_ADDRESS address, uint32_t size);
// a storage buffer: a raw (ByteAddressBuffer) UAV; offset and size in bytes, offset 16-aligned
void d3d12_set_write_buffer(const d3d12_set_t *set, unsigned binding, unsigned element,
                            ID3D12Resource *buffer, uint64_t offset, uint64_t size);
// a storage image; desc NULL = the resource's own format, mip 0
void d3d12_set_write_uav_image(const d3d12_set_t *set, unsigned binding, unsigned element,
                               ID3D12Resource *image, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc);
// a combined image sampler: the view and its sampler; desc NULL = the resource's own format, every mip
void d3d12_set_write_srv_image(const d3d12_set_t *set, unsigned binding, unsigned element,
                               ID3D12Resource *image, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                               const D3D12_SAMPLER_DESC *sampler);
void d3d12_set_write_texel_buffer(const d3d12_set_t *set, unsigned binding, unsigned element,
                                  ID3D12Resource *buffer, DXGI_FORMAT format, uint64_t first, uint32_t count);
void d3d12_set_write_as(const d3d12_set_t *set, unsigned binding, unsigned element, D3D12_GPU_VIRTUAL_ADDRESS tlas);
void d3d12_set_write_sampler(const d3d12_set_t *set, unsigned binding, unsigned element,
                             const D3D12_SAMPLER_DESC *sampler);

/* The four samplers of D3D12_GLOBAL_SAMPLER_* (vkpt's tex_sampler,
   tex_sampler_nearest, ..._nearest_mipmap_aniso, ..._linear_clamp). */
void d3d12_global_sampler_desc(int kind, D3D12_SAMPLER_DESC *desc);

/* ------------------------------------------------------------------------- */
/* COMPUTE: binding sets and dispatching against a pipeline layout            */
/* ------------------------------------------------------------------------- */

// a pass's PSO, from its DXIL, against its layout's root signature
// SER (pt_ser): a DXIL library's ray generation shader "main" (spv2dxil --raygen / --ser)
// in a state object with `layout`'s root signature as the global one and no hit groups -
// the shader traces inline - and its shader identifier
// (D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES) into `identifier`; NULL on failure
ID3D12StateObject *d3d12_create_raygen_object(d3d12_pipeline_layout_id_t layout, const void *dxil, size_t size,
                                              void *identifier, const wchar_t *name);
ID3D12PipelineState *d3d12_create_compute_pso(d3d12_pipeline_layout_id_t layout, const void *dxil, size_t size,
                                              const wchar_t *name);

// SetComputeRootSignature and SetDescriptorHeaps, once per list (or after
// something else changed them)
void d3d12_compute_begin(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout);
// one set per entry of the layout's set list, in its order
void d3d12_compute_bind_sets(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                             const d3d12_set_t *const *sets);
void d3d12_compute_push_constants(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                                  const void *data, size_t size);
// Dispatch, with gl_NumWorkGroups set for the shaders that read it
void d3d12_compute_dispatch(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                            uint32_t x, uint32_t y, uint32_t z);

// The same for a draw (vkpt's final blits: final_blit.vert + a fragment shader
// against the UBO + textures layout). The root signatures allow every stage
// and need no input layout, so a graphics PSO is made against them as is.
void d3d12_graphics_begin(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout);
void d3d12_graphics_bind_sets(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                              const d3d12_set_t *const *sets);
void d3d12_graphics_push_constants(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                                   const void *data, size_t size);

/* ------------------------------------------------------------------------- */
/* THE G-BUFFER IMAGES (LIST_IMAGES)                                          */
/* ------------------------------------------------------------------------- */

/* vkpt's three image extents (qvk.extent_screen_images, extent_unscaled,
   extent_taa_images); one GPU, so IMG_WIDTH_MGPU is IMG_WIDTH. */
typedef struct {
    uint32_t screen_width, screen_height;       // the render resolution
    uint32_t unscaled_width, unscaled_height;   // the output resolution
    uint32_t taa_width, taa_height;
    // vkpt_screen_image_profile: the optional image groups this configuration
    // writes; the rest are made 1x1 (vkpt_screen_image_active), as vkpt makes them
    uint32_t profile;
} d3d12_extents_t;

/* The DXGI format of a G-buffer image (VKPT_IMG_* index), from LIST_IMAGES. */
DXGI_FORMAT d3d12_image_format(int image);
int d3d12_num_images(void);
const char *d3d12_image_name(int image);
// its size at these extents, from the same LIST_IMAGES expressions vkpt uses
void d3d12_image_extent(int image, const d3d12_extents_t *extents, uint32_t *width, uint32_t *height);
// the binding it sits at in the textures set (BINDING_OFFSET_IMAGES / _TEXTURES + this)
unsigned d3d12_image_binding(int image);
// the image the even (0) or odd (1) textures set shows at `image`'s binding:
// itself, or for the _A/_B pairs, the other half on odd frames (LIST_IMAGES_B_A)
int d3d12_image_at_binding(int image, int odd);
// which D3D12_GLOBAL_SAMPLER_* its combined image sampler uses (vkpt textures.c)
int d3d12_image_sampler(int image);

#endif // REFRESH_D3D12_BINDINGS_H
