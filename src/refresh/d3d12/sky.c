/*
The physical sky's GPU half - see sky.h.
*/

#include "sky.h"
#include "passes.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* DDS                                                                        */
/* ------------------------------------------------------------------------- */

// vkpt/dds.h's, the parts precomputed_sky.c reads
#define DDS_MAGIC_WORD          0x20534444u     // "DDS "
#define DDS_HEADER_BYTES        128u            // the magic and DDS_HEADER
#define DDS_DX10_BYTES          20u             // DDS_HEADER_DXT10
#define DDS_FOURCC_DX10         0x30315844u     // "DX10"
#define DDS_CAPS2_CUBEMAP       0x00000200u
#define DDS_CAPS2_ALLFACES      0x0000fe00u     // DDSCAPS2_CUBEMAP and its six faces
#define DDS_MISC_TEXTURECUBE    0x4u

static uint32_t u32_at(const uint8_t *p, size_t offset)
{
    uint32_t v;
    memcpy(&v, p + offset, sizeof(v));
    return v;
}

bool d3d12_dds_parse(const void *file, size_t file_size, d3d12_dds_t *dds, char *error, size_t error_size)
{
    const uint8_t *p = file;
    memset(dds, 0, sizeof(*dds));

    if (!p || file_size < DDS_HEADER_BYTES || u32_at(p, 0) != DDS_MAGIC_WORD || u32_at(p, 4) != DDS_HEADER_BYTES - 4) {
        snprintf(error, error_size, "not the expected DDS file format");
        return false;
    }

    const uint32_t height = u32_at(p, 12), width = u32_at(p, 16), depth = u32_at(p, 24);
    const uint32_t pf_flags = u32_at(p, 80), fourcc = u32_at(p, 84);
    const uint32_t masks[4] = { u32_at(p, 92), u32_at(p, 96), u32_at(p, 100), u32_at(p, 104) };
    const uint32_t caps2 = u32_at(p, 112);
    size_t header = DDS_HEADER_BYTES;
    (void)pf_flags;

    dds->layers = 1;
    if (fourcc == DDS_FOURCC_DX10) {
        if (file_size < DDS_HEADER_BYTES + DDS_DX10_BYTES) {
            snprintf(error, error_size, "a DX10 DDS file without its DX10 header");
            return false;
        }
        const uint32_t dxgi = u32_at(p, 128), misc = u32_at(p, 136), array_size = u32_at(p, 140);
        switch (dxgi) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:        // as R8G8B8A8, as LoadImageFromDDS does
            dds->format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dds->bytes_per_pixel = 4;
            break;
        case DXGI_FORMAT_R32_FLOAT:
            dds->format = DXGI_FORMAT_R32_FLOAT;
            dds->bytes_per_pixel = 4;
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            dds->format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            dds->bytes_per_pixel = 16;
            break;
        default:
            snprintf(error, error_size, "an unsupported pixel format (%u)", dxgi);
            return false;
        }
        dds->cube = (misc & DDS_MISC_TEXTURECUBE) != 0;
        dds->layers = (array_size ? array_size : 1) * (dds->cube ? 6 : 1);
        header += DDS_DX10_BYTES;
    } else {
        if ((caps2 & DDS_CAPS2_CUBEMAP) && (caps2 & DDS_CAPS2_ALLFACES) == DDS_CAPS2_ALLFACES) {
            dds->cube = true;
            dds->layers = 6;
        }
        if ((masks[0] == 0x00ff0000 && masks[1] == 0x0000ff00 && masks[2] == 0x000000ff && masks[3] == 0xff000000) ||
            (masks[0] == 0x000000ff && masks[1] == 0x0000ff00 && masks[2] == 0x00ff0000 && masks[3] == 0xff000000)) {
            dds->format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dds->bytes_per_pixel = 4;
        } else if (masks[0] == 0xffffffff && !masks[1] && !masks[2] && !masks[3]) {
            dds->format = DXGI_FORMAT_R32_FLOAT;
            dds->bytes_per_pixel = 4;
        } else {
            snprintf(error, error_size, "an unsupported pixel format (masks %08x %08x %08x %08x)", masks[0], masks[1],
                     masks[2], masks[3]);
            return false;
        }
    }

    dds->width = width;
    dds->height = height;
    dds->depth = depth ? depth : 1;
    if (!width || !height || (dds->depth > 1 && dds->layers > 1)) {
        snprintf(error, error_size, "an unsupported shape (%ux%ux%u, %u layers)", width, height, dds->depth,
                 dds->layers);
        return false;
    }
    dds->size = (size_t)width * height * dds->depth * dds->layers * dds->bytes_per_pixel;
    if (file_size - header < dds->size) {
        snprintf(error, error_size, "%zu bytes of pixels, %zu expected", file_size - header, dds->size);
        return false;
    }
    dds->pixels = p + header;
    return true;
}

void d3d12_dds_resource_desc(const d3d12_dds_t *dds, D3D12_RESOURCE_DESC *rd)
{
    memset(rd, 0, sizeof(*rd));
    rd->Dimension = dds->depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd->Width = dds->width;
    rd->Height = dds->height;
    rd->DepthOrArraySize = (UINT16)(dds->depth > 1 ? dds->depth : dds->layers);
    rd->MipLevels = 1;
    rd->Format = dds->format;
    rd->SampleDesc.Count = 1;
}

void d3d12_dds_srv_desc(const d3d12_dds_t *dds, D3D12_SHADER_RESOURCE_VIEW_DESC *sv)
{
    memset(sv, 0, sizeof(*sv));
    sv->Format = dds->format;
    sv->Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (dds->depth > 1) {
        sv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv->Texture3D.MipLevels = 1;
    } else if (dds->cube) {
        sv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv->TextureCube.MipLevels = 1;
    } else if (dds->layers > 1) {
        sv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv->Texture2DArray.MipLevels = 1;
        sv->Texture2DArray.ArraySize = dds->layers;
    } else {
        sv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv->Texture2D.MipLevels = 1;
    }
}

/* ------------------------------------------------------------------------- */
/* IMAGES                                                                     */
/* ------------------------------------------------------------------------- */

static void cube_desc(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_DESC *rd)
{
    memset(rd, 0, sizeof(*rd));
    rd->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd->Width = width;
    rd->Height = height;
    rd->DepthOrArraySize = 6;
    rd->MipLevels = 1;
    rd->Format = format;
    rd->SampleDesc.Count = 1;
}

static void cube_srv_desc(DXGI_FORMAT format, D3D12_SHADER_RESOURCE_VIEW_DESC *sv)
{
    memset(sv, 0, sizeof(*sv));
    sv->Format = format;
    sv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    sv->Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv->TextureCube.MipLevels = 1;
}

void d3d12_sky_envmap_desc(D3D12_RESOURCE_DESC *rd)
{
    cube_desc(D3D12_SKY_ENVMAP_SIZE, D3D12_SKY_ENVMAP_SIZE, D3D12_SKY_ENVMAP_FORMAT, rd);
    rd->Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
}

void d3d12_sky_envmap_srv_desc(D3D12_SHADER_RESOURCE_VIEW_DESC *sv)
{
    cube_srv_desc(D3D12_SKY_ENVMAP_FORMAT, sv);
}

void d3d12_sky_envmap_uav_desc(D3D12_UNORDERED_ACCESS_VIEW_DESC *uv)
{
    memset(uv, 0, sizeof(*uv));
    uv->Format = D3D12_SKY_ENVMAP_FORMAT;
    uv->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uv->Texture2DArray.ArraySize = 6;
}

void d3d12_sky_skybox_desc(uint32_t width, uint32_t height, D3D12_RESOURCE_DESC *rd)
{
    cube_desc(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, rd);
}

void d3d12_sky_skybox_srv_desc(D3D12_SHADER_RESOURCE_VIEW_DESC *sv)
{
    cube_srv_desc(DXGI_FORMAT_R8G8B8A8_UNORM, sv);
}

void d3d12_sky_shadowmap_desc(D3D12_RESOURCE_DESC *rd, D3D12_CLEAR_VALUE *clear)
{
    memset(rd, 0, sizeof(*rd));
    rd->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd->Width = D3D12_SKY_SHADOWMAP_SIZE;
    rd->Height = D3D12_SKY_SHADOWMAP_SIZE;
    rd->DepthOrArraySize = 1;
    rd->MipLevels = 1;
    rd->Format = DXGI_FORMAT_R32_TYPELESS;      // D32 to draw, R32 to sample
    rd->SampleDesc.Count = 1;
    rd->Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    memset(clear, 0, sizeof(*clear));
    clear->Format = DXGI_FORMAT_D32_FLOAT;
    clear->DepthStencil.Depth = 1.f;
}

void d3d12_sky_shadowmap_srv_desc(D3D12_SHADER_RESOURCE_VIEW_DESC *sv)
{
    memset(sv, 0, sizeof(*sv));
    sv->Format = DXGI_FORMAT_R32_FLOAT;
    sv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv->Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv->Texture2D.MipLevels = 1;
}

void d3d12_sky_shadowmap_dsv_desc(D3D12_DEPTH_STENCIL_VIEW_DESC *dv)
{
    memset(dv, 0, sizeof(*dv));
    dv->Format = DXGI_FORMAT_D32_FLOAT;
    dv->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
}

void d3d12_sky_shadowmap_pso_desc(D3D12_GRAPHICS_PIPELINE_STATE_DESC *pd, D3D12_INPUT_ELEMENT_DESC *element,
                                  ID3D12RootSignature *root_signature, const void *vs, size_t vs_size)
{
    memset(element, 0, sizeof(*element));
    // SPIRV-Cross names location N's input TEXCOORDN
    element->SemanticName = "TEXCOORD";
    element->SemanticIndex = 0;
    element->Format = DXGI_FORMAT_R32G32B32_FLOAT;
    element->InputSlot = 0;
    element->AlignedByteOffset = 0;
    element->InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    memset(pd, 0, sizeof(*pd));
    pd->pRootSignature = root_signature;
    pd->VS.pShaderBytecode = vs;
    pd->VS.BytecodeLength = vs_size;
    pd->SampleMask = UINT_MAX;
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd->RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    pd->RasterizerState.FrontCounterClockwise = FALSE;
    pd->RasterizerState.DepthClipEnable = TRUE;
    pd->DepthStencilState.DepthEnable = TRUE;
    pd->DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd->DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd->InputLayout.pInputElementDescs = element;
    pd->InputLayout.NumElements = 1;
    pd->PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd->NumRenderTargets = 0;
    pd->DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd->SampleDesc.Count = 1;
}

/* ------------------------------------------------------------------------- */
/* RECORDING                                                                  */
/* ------------------------------------------------------------------------- */

static void sky_transition(ID3D12GraphicsCommandList4 *cmd, ID3D12Resource *r, D3D12_RESOURCE_STATES before,
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

// precomputed_sky.c's RecordCommandBufferShadowmap
static void record_shadowmap(const d3d12_sky_pass_t *s)
{
    ID3D12GraphicsCommandList4 *cmd = s->cmd;
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cmd;

    sky_transition(cmd, s->shadowmap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ID3D12GraphicsCommandList4_ClearDepthStencilView(cmd, s->shadowmap_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, NULL);

    if (s->grid_index_count && s->shadowmap_pso) {
        ID3D12GraphicsCommandList4_SetPipelineState(cmd, s->shadowmap_pso);
        d3d12_graphics_begin(gl, D3D12_PL_SHADOW_MAP);
        d3d12_graphics_push_constants(gl, D3D12_PL_SHADOW_MAP, s->viewproj, sizeof(s->viewproj));
        const D3D12_VIEWPORT vp = { 0.f, 0.f, (float)D3D12_SKY_SHADOWMAP_SIZE, (float)D3D12_SKY_SHADOWMAP_SIZE, 0.f, 1.f };
        const D3D12_RECT sc = { 0, 0, D3D12_SKY_SHADOWMAP_SIZE, D3D12_SKY_SHADOWMAP_SIZE };
        ID3D12GraphicsCommandList4_RSSetViewports(cmd, 1, &vp);
        ID3D12GraphicsCommandList4_RSSetScissorRects(cmd, 1, &sc);
        ID3D12GraphicsCommandList4_OMSetRenderTargets(cmd, 0, NULL, FALSE, &s->shadowmap_dsv);
        ID3D12GraphicsCommandList4_IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList4_IASetVertexBuffers(cmd, 0, 1, &s->grid_vertices);
        ID3D12GraphicsCommandList4_IASetIndexBuffer(cmd, &s->grid_indices);
        ID3D12GraphicsCommandList4_DrawIndexedInstanced(cmd, s->grid_index_count, 1, 0, 0, 0);
    }

    sky_transition(cmd, s->shadowmap, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void d3d12_record_sky(const d3d12_sky_pass_t *s)
{
    ID3D12GraphicsCommandList4 *cmd = s->cmd;
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cmd;

    if (s->shadowmap)
        record_shadowmap(s);

    // reset_sun_color_buffer: the sky pass accumulates into it
    d3d12_clear_buffer(cmd, s->sun_color_buffer, s->sun_color_size, s->zero_buffer);

    // the envmap's six faces, 16x16 groups
    ID3D12GraphicsCommandList4_SetPipelineState(cmd, s->sky_pso);
    d3d12_compute_begin(gl, D3D12_PL_PHYSICAL_SKY);
    d3d12_compute_bind_sets(gl, D3D12_PL_PHYSICAL_SKY, s->sets);
    d3d12_compute_push_constants(gl, D3D12_PL_PHYSICAL_SKY, s->viewproj, sizeof(s->viewproj));
    d3d12_compute_dispatch(gl, D3D12_PL_PHYSICAL_SKY, D3D12_SKY_ENVMAP_SIZE / 16, D3D12_SKY_ENVMAP_SIZE / 16, 6);
    d3d12_barrier_all(cmd, s->enhanced_barriers);

    // the accumulated sun and sky colours into the lighting's constants
    const d3d12_set_t *resolve_sets[2] = { s->sets[0], s->sets[2] };
    ID3D12GraphicsCommandList4_SetPipelineState(cmd, s->resolve_pso);
    d3d12_compute_begin(gl, D3D12_PL_UBO_VERTEX_BUFFER);
    d3d12_compute_bind_sets(gl, D3D12_PL_UBO_VERTEX_BUFFER, resolve_sets);
    d3d12_compute_dispatch(gl, D3D12_PL_UBO_VERTEX_BUFFER, 1, 1, 1);
    d3d12_barrier_all(cmd, s->enhanced_barriers);

    // read as SUN_COLOR_UBO from here on; it decays to COMMON when the frame's
    // list is done, from which the next frames' reads promote it
    sky_transition(cmd, s->sun_color_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
}
