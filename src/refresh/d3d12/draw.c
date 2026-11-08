/*
D3D12 2D layer: the HUD, console and menus.

vkpt's R_Draw*_RTX functions fill the queue (vkpt/draw.c - borrowed whole, see
frontend.c) and this draws it with the same two shaders vkpt uses,
stretch_pic.vert and stretch_pic.frag, translated to DXIL. One instanced draw:
4 vertices (a strip) per queued pic.

The root signature is the translated shaders' bindings, by the rule in
d3d12pt.h (register 0, space 1000 * (set + 1) + binding):

    set 0 binding 0     SBO, the queue          u0 space1000    root UAV
    set 1 binding 0     bindless images         t0 space2000    table, unbounded
    set 1 binding 900   the four samplers       s0 space2900    table
    set 1 binding 901   sampler per image       u0 space2901    root UAV
    set 2 binding 2     UBO (HDR settings)      b0 space3002    root CBV

Storage buffers are always UAVs, and an UPLOAD heap buffer cannot be one, so
the queue is staged in an upload buffer and copied into a DEFAULT buffer by the
frame's own command list.

Two pipelines, as vkpt's STRETCH_PIC_SDR / _HDR: with vid_hdr the frame target is
scRGB and stretch_pic.frag.spec_0_1 (spec_tone_mapping_hdr 1) draws into it, reading
the UBO - the UI's nits, its saturation and the video frames' curve
(ui_hdr_video_gamma), from vkpt_draw_ui_hdr_params.
*/

#include "d3d12pt.h"

enum {
    ROOT_SBO,
    ROOT_TEXTURES,
    ROOT_SAMPLER_INDEX,
    ROOT_UBO,
    ROOT_SAMPLERS,
    NUM_ROOT_PARAMS
};

#define UBO_SIZE    256     // a CBV is 256-byte aligned; the queue follows it

typedef struct {
    float ui_hdr_nits;
    float tm_hdr_saturation_scale;
    float ui_hdr_video_gamma;
    float ui_hdr_gamma;
} StretchPic_UBO_t;        // stretch_pic.frag's UBO

static ID3D12RootSignature  *root_signature;
static ID3D12PipelineState  *pipelines[2];  // [d3d.hdr]: into the sRGB view, into the scRGB target

static struct {
    ID3D12Resource  *staging;       // UPLOAD: UBO, then the queue
    uint8_t         *staging_map;
    ID3D12Resource  *sbo;           // DEFAULT, UAV: the queue the vertex shader reads
    uint64_t        uploaded;       // d3d.frame_counter + 1 of the frame it holds the queue of
} per_frame[D3D12_FRAMES_IN_FLIGHT];

static size_t queue_bytes;          // the most the queue can hold

static bool create_root_signature(void)
{
    D3D12_DESCRIPTOR_RANGE1 textures = { 0 };
    textures.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textures.NumDescriptors = UINT_MAX;     // unbounded, like the GLSL array
    textures.BaseShaderRegister = 0;
    textures.RegisterSpace = D3D12_SPACE(1, 0);
    // images come and go while the table is bound: bindless
    textures.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    textures.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE1 samplers = { 0 };
    samplers.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplers.NumDescriptors = D3D12_NUM_GLOBAL_SAMPLERS;
    samplers.BaseShaderRegister = 0;
    samplers.RegisterSpace = D3D12_SPACE(1, D3D12_BINDING_GLOBAL_SAMPLERS);
    samplers.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    samplers.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 p[NUM_ROOT_PARAMS];
    memset(p, 0, sizeof(p));

    p[ROOT_SBO].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[ROOT_SBO].Descriptor.ShaderRegister = 0;
    p[ROOT_SBO].Descriptor.RegisterSpace = D3D12_SPACE(0, 0);
    p[ROOT_SBO].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    p[ROOT_SBO].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    p[ROOT_TEXTURES].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_TEXTURES].DescriptorTable.NumDescriptorRanges = 1;
    p[ROOT_TEXTURES].DescriptorTable.pDescriptorRanges = &textures;
    p[ROOT_TEXTURES].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    p[ROOT_SAMPLER_INDEX].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[ROOT_SAMPLER_INDEX].Descriptor.ShaderRegister = 0;
    p[ROOT_SAMPLER_INDEX].Descriptor.RegisterSpace = D3D12_SPACE(1, D3D12_BINDING_GLOBAL_SAMPLER_INDEX);
    p[ROOT_SAMPLER_INDEX].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    p[ROOT_SAMPLER_INDEX].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    p[ROOT_UBO].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[ROOT_UBO].Descriptor.ShaderRegister = 0;
    p[ROOT_UBO].Descriptor.RegisterSpace = D3D12_SPACE(2, 2);
    p[ROOT_UBO].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    p[ROOT_UBO].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    p[ROOT_SAMPLERS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_SAMPLERS].DescriptorTable.NumDescriptorRanges = 1;
    p[ROOT_SAMPLERS].DescriptorTable.pDescriptorRanges = &samplers;
    p[ROOT_SAMPLERS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = { 0 };
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = NUM_ROOT_PARAMS;
    desc.Desc_1_1.pParameters = p;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                          D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                          D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ID3DBlob *blob = NULL, *error = NULL;
    HRESULT hr = d3d12_serialize_root_signature(&desc, &blob, &error);
    if (FAILED(hr)) {
        Com_EPrintf("D3D12: 2D root signature: %s\n",
                    error ? (const char *)ID3D10Blob_GetBufferPointer(error) : va("0x%08lx", (unsigned long)hr));
        D3D12_RELEASE(error);
        return false;
    }
    D3D12_RELEASE(error);
    hr = ID3D12Device5_CreateRootSignature(d3d.device, 0, ID3D10Blob_GetBufferPointer(blob),
                                           ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                           (void **)&root_signature);
    D3D12_RELEASE(blob);
    if (!D3D_CHECK(hr))
        return false;
    d3d12_set_name((ID3D12Object *)root_signature, "2D root signature");
    return true;
}

static bool create_pipeline(int hdr)
{
    size_t vs_size = 0, ps_size = 0;
    ID3D12PipelineState *pipeline = NULL;
    void *vs = d3d12_load_shader("stretch_pic.vert", &vs_size);
    void *ps = d3d12_load_shader(hdr ? "stretch_pic.frag.spec_0_1" : "stretch_pic.frag", &ps_size);
    if (!vs || !ps) {
        if (vs) FS_FreeFile(vs);
        if (ps) FS_FreeFile(ps);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = { 0 };
    pd.pRootSignature = root_signature;
    pd.VS.pShaderBytecode = vs;
    pd.VS.BytecodeLength = vs_size;
    pd.PS.pShaderBytecode = ps;
    pd.PS.BytecodeLength = ps_size;

    // vkpt's: straight alpha over, colour and alpha alike
    D3D12_RENDER_TARGET_BLEND_DESC *bl = &pd.BlendState.RenderTarget[0];
    bl->BlendEnable = TRUE;
    bl->SrcBlend = D3D12_BLEND_SRC_ALPHA;
    bl->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    bl->BlendOp = D3D12_BLEND_OP_ADD;
    bl->SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
    bl->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    bl->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bl->LogicOp = D3D12_LOGIC_OP_NOOP;
    bl->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;

    // vkpt culls back faces; spv2dxil flips y for D3D's clip space, which
    // mirrors every quad's winding - and R_DrawRotatedPic's skew can mirror it
    // again - so cull nothing
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;

    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = hdr ? D3D12_HDR_FORMAT : D3D12_FRAME_TARGET_RTV_FORMAT;
    pd.SampleDesc.Count = 1;

    HRESULT hr = ID3D12Device5_CreateGraphicsPipelineState(d3d.device, &pd, &IID_ID3D12PipelineState,
                                                           (void **)&pipeline);
    FS_FreeFile(vs);
    FS_FreeFile(ps);
    if (!D3D_CHECK(hr))
        return false;
    d3d12_set_name((ID3D12Object *)pipeline, hdr ? "stretch_pic HDR" : "stretch_pic");
    pipelines[hdr] = pipeline;
    return true;
}

bool d3d12_draw_init(void)
{
    int count, max_count;
    size_t record;
    d3d12_frontend_stretch_pics(&count, &record, &max_count);
    queue_bytes = record * (size_t)max_count;

    if (!create_root_signature() || !create_pipeline(0) || !create_pipeline(1))
        return false;

    for (int i = 0; i < D3D12_FRAMES_IN_FLIGHT; i++) {
        per_frame[i].staging = d3d12_create_buffer(UBO_SIZE + queue_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                                   D3D12_RESOURCE_FLAG_NONE, "2D staging");
        per_frame[i].sbo = d3d12_create_buffer(queue_bytes, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "2D queue");
        if (!per_frame[i].staging || !per_frame[i].sbo)
            return false;
        D3D12_RANGE none = { 0, 0 };
        void *map = NULL;
        if (!D3D_CHECK(ID3D12Resource_Map(per_frame[i].staging, 0, &none, &map)))
            return false;
        per_frame[i].staging_map = map;
    }
    return true;
}

void d3d12_draw_shutdown(void)
{
    // the GPU is idle here
    for (int i = 0; i < D3D12_FRAMES_IN_FLIGHT; i++) {
        D3D12_RELEASE(per_frame[i].staging);
        D3D12_RELEASE(per_frame[i].sbo);
        per_frame[i].staging_map = NULL;
        per_frame[i].uploaded = 0;     // the frame counter may start again
    }
    D3D12_RELEASE(pipelines[0]);
    D3D12_RELEASE(pipelines[1]);
    D3D12_RELEASE(root_signature);
}

void d3d12_draw_submit(ID3D12GraphicsCommandList4 *cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, int width, int height)
{
    int count, max_count;
    size_t record;
    const void *queue = d3d12_frontend_stretch_pics(&count, &record, &max_count);
    ID3D12PipelineState *pipeline = pipelines[d3d.hdr ? 1 : 0];
    if (count <= 0 || !pipeline)
        return;

    // the frame's own slice: the GPU finished with it before R_BeginFrame
    // handed this frame index out again
    int f = d3d.frame_index;
    // Frame generation draws the same queue over each generated frame too (R_EndFrame):
    // the first call of the frame uploads it, the others draw from that - the SBO is
    // in UNORDERED_ACCESS by then, and a second copy into it would need it back
    if (per_frame[f].uploaded != d3d.frame_counter + 1) {
        per_frame[f].uploaded = d3d.frame_counter + 1;
        size_t bytes = record * (size_t)count;
        // read by the HDR pipeline only, as vkpt's
        StretchPic_UBO_t ubo;
        d3d12_frontend_ui_hdr_params(&ubo.ui_hdr_nits, &ubo.tm_hdr_saturation_scale, &ubo.ui_hdr_video_gamma);
        ubo.ui_hdr_gamma = d3d12_frontend_ui_hdr_gamma();
        memcpy(per_frame[f].staging_map, &ubo, sizeof(ubo));
        memcpy(per_frame[f].staging_map + UBO_SIZE, queue, bytes);

        // COMMON (decayed) -> COPY_DEST by promotion, then explicitly to UAV
        ID3D12GraphicsCommandList4_CopyBufferRegion(cmd, per_frame[f].sbo, 0, per_frame[f].staging, UBO_SIZE, bytes);
        D3D12_RESOURCE_BARRIER b = { 0 };
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = per_frame[f].sbo;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12GraphicsCommandList4_ResourceBarrier(cmd, 1, &b);
    }

    ID3D12DescriptorHeap *heaps[2] = { d3d.srv_heap, d3d.sampler_heap };
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cmd, 2, heaps);
    ID3D12GraphicsCommandList4_SetGraphicsRootSignature(cmd, root_signature);
    ID3D12GraphicsCommandList4_SetPipelineState(cmd, pipeline);

    ID3D12GraphicsCommandList4_SetGraphicsRootUnorderedAccessView(cmd, ROOT_SBO,
        ID3D12Resource_GetGPUVirtualAddress(per_frame[f].sbo));
    ID3D12GraphicsCommandList4_SetGraphicsRootDescriptorTable(cmd, ROOT_TEXTURES,
        d3d12_srv_gpu(d3d12_bindless_base((int)(d3d.frame_counter & 1))));     // this frame's copy (textures.c)
    ID3D12GraphicsCommandList4_SetGraphicsRootUnorderedAccessView(cmd, ROOT_SAMPLER_INDEX,
        d3d12_sampler_index_buffer());
    ID3D12GraphicsCommandList4_SetGraphicsRootConstantBufferView(cmd, ROOT_UBO,
        ID3D12Resource_GetGPUVirtualAddress(per_frame[f].staging));
    ID3D12GraphicsCommandList4_SetGraphicsRootDescriptorTable(cmd, ROOT_SAMPLERS, d3d12_sampler_gpu(0));

    D3D12_VIEWPORT vp = { 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
    D3D12_RECT sc = { 0, 0, width, height };
    ID3D12GraphicsCommandList4_RSSetViewports(cmd, 1, &vp);
    ID3D12GraphicsCommandList4_RSSetScissorRects(cmd, 1, &sc);
    ID3D12GraphicsCommandList4_OMSetRenderTargets(cmd, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList4_IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList4_DrawInstanced(cmd, 4, (UINT)count, 0, 0);
}
