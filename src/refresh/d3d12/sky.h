/*
The physical sky's GPU half, engine-free like passes.c (tests/d3d12_world_test.c
compiles it in): the DDS tables vkpt's precomputed_sky.c loads, the images and
the terrain shadow map's pipeline as vkpt makes them, and the recording of
vkpt_physical_sky_record_cmd_buffer. world.c owns the resources and the files;
frontend.c hands it what is vkpt's (the preset's atmosphere, the terrain grid,
the shadow map's view, when the sky needs rendering).
*/

#ifndef D3D12_SKY_H
#define D3D12_SKY_H

#include "bindings.h"

/* A DDS file as precomputed_sky.c's LoadImageFromDDS reads it, bug for bug:
   B8G8R8A8 data (DX10 or legacy bit masks of either order) is made R8G8B8A8,
   so its red and blue stay swapped as they are on Vulkan, where the sky's
   look was tuned. */
typedef struct {
    DXGI_FORMAT     format;
    uint32_t        width, height;
    uint32_t        depth;              // > 1: a 3D image
    uint32_t        layers;             // 6 per cube
    bool            cube;
    uint32_t        bytes_per_pixel;
    const uint8_t   *pixels;            // layer after layer (a 3D image: slice after slice), tightly packed
    size_t          size;               // width * height * depth * layers * bytes_per_pixel
} d3d12_dds_t;

bool d3d12_dds_parse(const void *file, size_t file_size, d3d12_dds_t *dds, char *error, size_t error_size);
void d3d12_dds_resource_desc(const d3d12_dds_t *dds, D3D12_RESOURCE_DESC *rd);
void d3d12_dds_srv_desc(const d3d12_dds_t *dds, D3D12_SHADER_RESOURCE_VIEW_DESC *sv);

/* physical_sky.c's envmap: a cube of rgba16f the sky pass renders whole,
   TEX_PHYSICAL_SKY as a cube and IMG_PHYSICAL_SKY as its six faces (HLSL has
   no writable cube). Storage and sampled with no state changes, as the G-buffer. */
#define D3D12_SKY_ENVMAP_SIZE       1024
#define D3D12_SKY_ENVMAP_FORMAT     DXGI_FORMAT_R16G16B16A16_FLOAT
void d3d12_sky_envmap_desc(D3D12_RESOURCE_DESC *rd);
void d3d12_sky_envmap_srv_desc(D3D12_SHADER_RESOURCE_VIEW_DESC *sv);
void d3d12_sky_envmap_uav_desc(D3D12_UNORDERED_ACCESS_VIEW_DESC *uv);

/* R_SetSky's skybox, TEX_ENVMAP: six RGBA8 faces (textures.c's
   vkpt_textures_upload_envmap) */
void d3d12_sky_skybox_desc(uint32_t width, uint32_t height, D3D12_RESOURCE_DESC *rd);
void d3d12_sky_skybox_srv_desc(D3D12_SHADER_RESOURCE_VIEW_DESC *sv);

/* The terrain shadow map (precomputed_sky.c): D32 drawn, R32 sampled. Kept in
   NON_PIXEL_SHADER_RESOURCE between sky updates; d3d12_record_sky takes it to
   DEPTH_WRITE and back. */
#define D3D12_SKY_SHADOWMAP_SIZE    2048
void d3d12_sky_shadowmap_desc(D3D12_RESOURCE_DESC *rd, D3D12_CLEAR_VALUE *clear);
void d3d12_sky_shadowmap_srv_desc(D3D12_SHADER_RESOURCE_VIEW_DESC *sv);
void d3d12_sky_shadowmap_dsv_desc(D3D12_DEPTH_STENCIL_VIEW_DESC *dv);
/* shadow_map.c's pipeline: shadow_map.vert alone, a float3 position at
   location 0, depth LESS, front faces culled (clockwise = front, as on Vulkan:
   spv2dxil's y flip leaves every triangle where Vulkan draws it). `element` is
   the input layout's storage. The root signature is D3D12_PL_SHADOW_MAP's. */
void d3d12_sky_shadowmap_pso_desc(D3D12_GRAPHICS_PIPELINE_STATE_DESC *pd, D3D12_INPUT_ELEMENT_DESC *element,
                                  ID3D12RootSignature *root_signature, const void *vs, size_t vs_size);

typedef struct {
    ID3D12GraphicsCommandList4  *cmd;
    bool                        enhanced_barriers;

    // the terrain shadow map; no grid (grid_index_count 0) leaves it cleared:
    // nothing in the sky is shadowed by the terrain
    ID3D12PipelineState         *shadowmap_pso;
    ID3D12Resource              *shadowmap;
    D3D12_CPU_DESCRIPTOR_HANDLE shadowmap_dsv;
    D3D12_VERTEX_BUFFER_VIEW    grid_vertices;
    D3D12_INDEX_BUFFER_VIEW     grid_indices;
    uint32_t                    grid_index_count;
    float                       viewproj[16];   // terrain_shadowmap_viewproj, column-major

    // physical_sky(_space).comp, then sky_buffer_resolve.comp
    ID3D12PipelineState         *sky_pso;
    ID3D12PipelineState         *resolve_pso;
    const d3d12_set_t           *sets[4];       // ubo, textures, vertex buffer, sky
    ID3D12Resource              *sun_color_buffer;
    uint64_t                    sun_color_size;
    ID3D12Resource              *zero_buffer;   // passes.h's zeros, the sun colour's clear
} d3d12_sky_pass_t;

/* vkpt_physical_sky_record_cmd_buffer, for a frame whose sky needs it: the
   shadow map, the sun colour cleared, the envmap and the sun colour rendered,
   resolved, and left a constant buffer for the lighting. Compute root state is
   left the sky's: the passes after it bind their own. */
void d3d12_record_sky(const d3d12_sky_pass_t *s);

#endif
