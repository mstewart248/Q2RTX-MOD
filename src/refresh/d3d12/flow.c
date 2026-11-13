/*
NVIDIA FLOW ON vid_rtx 3 - the D3D12 half of what vkpt/flow.c does with Vulkan.

vkpt/flow.c runs Flow itself, here through Flow's own D3D12 backend
(setup/flow/d3d12, built by setup/build_flow.py) on this renderer's device. Flow
records into command lists of its own and executes them on the frame's queue at
each flush - during frontend.c's d3d12_frontend_prepare_frame, so ahead of the
frame's list, which reads the density and the block table where Flow left them,
with no copy and no further synchronisation (NvFlowD3D12External.h says why).
This file is the rest, what flow.c makes and binds with Vulkan:

  * the buffers its shaders read: the FlowParams uniform buffer (UPLOAD, mapped,
    a slice a set - the frame's parity), the light sums (written by
    flow_lights.comp, then copied to a mapped READBACK copy, which flow.c reads
    two frames on: a READBACK buffer cannot be a UAV) and the occupancy;
  * the dummies that stand in for Flow's density texture and block table, and
    for the probe grid, in the textures set's Flow bindings;
  * the probe grid (pt_flow_env) and its pass's set per frame slot, which takes
    the slot's TLAS each frame;
  * the textures set's six Flow bindings, rewritten for the frame's set by
    vkpt_flow_update (d3d12_flow_bind) - the set's last frame has passed its
    fence by then.

The passes are passes.c's d3d12_record_flow and the path tracer's Flow variants;
world.c records them with the counts vkpt_flow_dispatches gives (frontend.c).
Like world.c this file reads no vkpt header but the shaders': frontend.c is the
glue (d3d12_flow_backend).
*/

#include "d3d12pt.h"
#include "passes.h"

#include "../vkpt/shader/global_textures.h"     // BINDING_OFFSET_FLOW_*
#include "../vkpt/shader/flow.h"                // FLOW_ENV_DIM_*, FLOW_BITSET_UVEC4S

#define FLOW_SETS   2       // flow.c's FLOW_SET_COUNT: the even and odd textures sets

static struct {
    bool            made;
    size_t          params_stride, light_sums_stride, occupancy_stride;
    ID3D12Resource  *params;                // UPLOAD, mapped: a FlowParams a set
    ID3D12Resource  *light_sums;            // DEFAULT, flow_lights.comp's
    ID3D12Resource  *light_sums_readback;   // READBACK, mapped: what the host reads
    ID3D12Resource  *occupancy;             // DEFAULT, flow_occupancy.comp's
    ID3D12Resource  *dummy_volume;          // RGBA16F 1x1x1 of zeros: the density's and the probes' stand-in
    ID3D12Resource  *dummy_table;           // 256 bytes, a UAV: the block table's (never read)

    // the probe grid: radiance (RGBA32F) and keys (RGBA32_UINT), storage and sampled
    // with no state changes, and the pass's set per frame slot
    ID3D12Resource  *env, *env_key;
    d3d12_set_t     env_sets[D3D12_FRAMES_IN_FLIGHT];
    bool            env_sets_made;          // this run's (they last as long as the descriptor heaps)
} fl;

static void release_all(void)
{
    D3D12_RELEASE(fl.params);
    D3D12_RELEASE(fl.light_sums);
    D3D12_RELEASE(fl.light_sums_readback);
    D3D12_RELEASE(fl.occupancy);
    D3D12_RELEASE(fl.dummy_volume);
    D3D12_RELEASE(fl.dummy_table);
    fl.made = false;
}

// a buffer of `heap`, mapped when it is an UPLOAD or READBACK one
static ID3D12Resource *buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, const char *name,
                              uint8_t **map)
{
    ID3D12Resource *b = d3d12_create_buffer(size, heap, flags, name);
    if (!b || !map)
        return b;
    // the upload buffer is never read back; the readback one is read in places
    D3D12_RANGE none = { 0, 0 };
    void *p = NULL;
    if (!D3D_CHECK(ID3D12Resource_Map(b, 0, heap == D3D12_HEAP_TYPE_UPLOAD ? &none : NULL, &p))) {
        D3D12_RELEASE(b);
        return NULL;
    }
    *map = p;
    return b;
}

// a 3D texture, storage (`uav`) and sampled with no state changes, as the G-buffer:
// simultaneous-access, in COMMON - zeroed in the upload batch ahead of the next frame
static ID3D12Resource *volume(uint32_t width, uint32_t height, uint32_t depth, DXGI_FORMAT format, bool uav,
                              const char *name)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = (UINT16)depth;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 0);
    ID3D12Resource *v = NULL;
    if (!D3D_CHECK(ID3D12Device5_CreateCommittedResource(d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                        D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource,
                                                        (void **)&v)))
        return NULL;
    d3d12_set_name((ID3D12Object *)v, name);
    if (!d3d12_world_zero_volume(v)) {
        D3D12_RELEASE(v);
        return NULL;
    }
    return v;
}

bool d3d12_flow_create(size_t params_stride, size_t light_sums_stride, size_t occupancy_stride,
                       uint8_t **params_mapped, uint8_t **light_sums_mapped)
{
    d3d12_flow_destroy();
    fl.params_stride = params_stride;
    fl.light_sums_stride = light_sums_stride;
    fl.occupancy_stride = occupancy_stride;

    // a CBV's address is 256-aligned, a raw UAV's offset 16-aligned: flow.c's strides
    // are multiples of 256
    if (!params_stride || !light_sums_stride || !occupancy_stride || (params_stride | light_sums_stride |
                                                                      occupancy_stride) % 256)
        return false;

    fl.params = buffer(params_stride * FLOW_SETS, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, "flow params",
                       params_mapped);
    fl.light_sums = buffer(light_sums_stride * FLOW_SETS, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "flow light sums", NULL);
    fl.light_sums_readback = buffer(light_sums_stride * FLOW_SETS, D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_FLAG_NONE, "flow light sums readback", light_sums_mapped);
    fl.occupancy = buffer(occupancy_stride * FLOW_SETS, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "flow occupancy", NULL);
    fl.dummy_table = buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            "flow dummy table", NULL);
    fl.dummy_volume = volume(1, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false, "flow dummy volume");

    if (!fl.params || !fl.light_sums || !fl.light_sums_readback || !fl.occupancy || !fl.dummy_table ||
        !fl.dummy_volume) {
        release_all();
        return false;
    }
    fl.made = true;
    return true;
}

void d3d12_flow_destroy(void)
{
    d3d12_flow_destroy_env();
    release_all();
    // the renderer is going, and its heaps with the sets
    fl.env_sets_made = false;
}

/* The probe grid: flow_ambient.comp loads and stores both images and the march
   samples the radiance, filtered. Ray query traced from a compute shader, which
   every DXR 1.1 device has; the formats' typed UAV loads are checked. */
static bool format_ok(DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 s1, D3D12_FORMAT_SUPPORT2 s2)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { format };
    if (FAILED(ID3D12Device5_CheckFeatureSupport(d3d.device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
        return false;
    return (fs.Support1 & s1) == s1 && (fs.Support2 & s2) == s2;
}

bool d3d12_flow_create_env(void)
{
    d3d12_flow_destroy_env();
    if (!fl.made)
        return false;

    const D3D12_FORMAT_SUPPORT2 storage = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if (!format_ok(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE,
                   storage) ||
        !format_ok(DXGI_FORMAT_R32G32B32A32_UINT, D3D12_FORMAT_SUPPORT1_TEXTURE3D, storage)) {
        Com_WPrintf("NVIDIA Flow: no filtered rgba32f storage volumes, so the smoke takes no light from the world\n");
        return false;
    }

    fl.env = volume(FLOW_ENV_DIM_X, FLOW_ENV_DIM_Y, FLOW_ENV_DIM_Z, DXGI_FORMAT_R32G32B32A32_FLOAT, true,
                    "flow ambient probes");
    fl.env_key = volume(FLOW_ENV_DIM_X, FLOW_ENV_DIM_Y, FLOW_ENV_DIM_Z, DXGI_FORMAT_R32G32B32A32_UINT, true,
                        "flow ambient keys");
    if (!fl.env || !fl.env_key) {
        d3d12_flow_destroy_env();
        return false;
    }

    // the images for good; the TLAS every frame (d3d12_flow_frame). The sets live as
    // long as the descriptor heaps - this renderer's run.
    for (int i = 0; i < D3D12_FRAMES_IN_FLIGHT && !fl.env_sets_made; i++)
        if (!d3d12_set_alloc(&fl.env_sets[i], D3D12_SET_FLOW_ENV, "flow env")) {
            d3d12_flow_destroy_env();
            return false;
        }
    fl.env_sets_made = true;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = { 0 };
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uv.Texture3D.WSize = FLOW_ENV_DIM_Z;
    for (int i = 0; i < D3D12_FRAMES_IN_FLIGHT; i++) {
        uv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d3d12_set_write_uav_image(&fl.env_sets[i], 1, 0, fl.env, &uv);
        uv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        d3d12_set_write_uav_image(&fl.env_sets[i], 2, 0, fl.env_key, &uv);
    }
    return true;
}

void d3d12_flow_destroy_env(void)
{
    D3D12_RELEASE(fl.env);
    D3D12_RELEASE(fl.env_key);
}

// a 3D texture's sampled view, mip 0; `format` 0 = the resource's own
static void bind_volume(const d3d12_set_t *set, unsigned binding, ID3D12Resource *v, DXGI_FORMAT format,
                        const D3D12_SAMPLER_DESC *sd)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    if (format == DXGI_FORMAT_UNKNOWN) {
        D3D12_RESOURCE_DESC rd;
        ID3D12Resource_GetDesc(v, &rd);
        format = rd.Format;
    }
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture3D.MipLevels = 1;
    d3d12_set_write_srv_image(set, binding, 0, v, &sv, sd);
}

void d3d12_flow_bind(int set, ID3D12Resource *density, DXGI_FORMAT density_format, ID3D12Resource *table)
{
    const d3d12_set_t *s = d3d12_world_textures_set(set & 1);
    if (!fl.made || !s)
        return;

    // Flow's: a 3D texture, and a buffer it writes (its views read it raw); the
    // dummies when there is no grid this frame (FlowParams.block_dim_bits.w is 0)
    if (density && table) {
        D3D12_RESOURCE_DESC dd, td;
        ID3D12Resource_GetDesc(density, &dd);
        ID3D12Resource_GetDesc(table, &td);
        if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || td.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            !(td.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
            static bool warned;
            if (!warned)
                Com_WPrintf("NVIDIA Flow: its density or table is not what the march reads - not drawn\n");
            warned = true;
            density = table = NULL;
        }
    }
    D3D12_SAMPLER_DESC linear_clamp;
    d3d12_global_sampler_desc(D3D12_GLOBAL_SAMPLER_LINEAR_CLAMP, &linear_clamp);
    if (density && table) {
        bind_volume(s, BINDING_OFFSET_FLOW_DENSITY, density, density_format, &linear_clamp);
        D3D12_RESOURCE_DESC td;
        ID3D12Resource_GetDesc(table, &td);
        d3d12_set_write_buffer(s, BINDING_OFFSET_FLOW_TABLE, 0, table, 0, td.Width);
    } else {
        bind_volume(s, BINDING_OFFSET_FLOW_DENSITY, fl.dummy_volume, DXGI_FORMAT_UNKNOWN, &linear_clamp);
        d3d12_set_write_buffer(s, BINDING_OFFSET_FLOW_TABLE, 0, fl.dummy_table, 0, 256);
    }

    const uint64_t i = (uint64_t)(set & 1);
    d3d12_set_write_cbv(s, BINDING_OFFSET_FLOW_PARAMS, 0,
                        ID3D12Resource_GetGPUVirtualAddress(fl.params) + i * fl.params_stride,
                        (uint32_t)fl.params_stride);
    d3d12_set_write_buffer(s, BINDING_OFFSET_FLOW_LIGHTS, 0, fl.light_sums, i * fl.light_sums_stride,
                           fl.light_sums_stride);
    d3d12_set_write_buffer(s, BINDING_OFFSET_FLOW_OCCUPANCY, 0, fl.occupancy, i * fl.occupancy_stride,
                           fl.occupancy_stride);

    // the probes, wrapping like the grid they hold (vkpt's tex_sampler, which wraps;
    // linear, as a volume of one mip has no use for its anisotropy)
    D3D12_SAMPLER_DESC wrap = { 0 };
    wrap.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    wrap.AddressU = wrap.AddressV = wrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    wrap.MaxAnisotropy = 1;
    wrap.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    wrap.MaxLOD = D3D12_FLOAT32_MAX;
    bind_volume(s, BINDING_OFFSET_FLOW_AMBIENT, fl.env ? fl.env : fl.dummy_volume, DXGI_FORMAT_UNKNOWN, &wrap);
}

const d3d12_set_t *d3d12_flow_frame(const d3d12_world_flow_t *flow, int slot, D3D12_GPU_VIRTUAL_ADDRESS tlas,
                                    d3d12_flow_passes_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!fl.made || !flow)
        return NULL;
    const uint64_t i = (uint64_t)(flow->set & 1);

    out->occupancy = fl.occupancy;
    out->occupancy_offset = i * fl.occupancy_stride;
    // flow.c's layout of a slice: a uvec4 a block, then the group bitset and the
    // block bitset - all of the slice that is not the blocks' (flow_set_strides
    // makes that exact: FLOW_POOL_MAX blocks and the bitsets fill whole 256s)
    out->group_bitset_size = FLOW_BITSET_UVEC4S * 16;
    out->occupancy_capacity = (uint32_t)((fl.occupancy_stride - out->group_bitset_size) / 16);
    out->occupancy_blocks = flow->occupancy_blocks;
    out->light_sums = fl.light_sums;
    out->light_sums_readback = fl.light_sums_readback;
    out->light_sums_offset = i * fl.light_sums_stride;
    out->light_sums_size = fl.light_sums_stride;
    out->light_blocks = flow->light_blocks;

    if (!fl.env || !tlas || slot < 0 || slot >= D3D12_FRAMES_IN_FLIGHT || !flow->env_groups[0] ||
        !flow->env_groups[1] || !flow->env_groups[2])
        return NULL;
    memcpy(out->env_groups, flow->env_groups, sizeof(out->env_groups));
    // the slot's TLAS, every frame: its last frame has passed its fence
    d3d12_set_write_as(&fl.env_sets[slot], 0, 0, tlas);
    return &fl.env_sets[slot];
}
