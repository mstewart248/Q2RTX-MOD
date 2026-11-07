/*
The binding layer: vkpt's set layouts described for D3D12, and one root
signature per pipeline layout. See bindings.h.
*/

#include "bindings.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The shader headers, in C mode: binding numbers and the image list. */
#include "../vkpt/shader/constants.h"
#ifndef MAX_RIMAGES
// global_textures.h checks this against NUM_GLOBAL_TEXTUES; the engine gets it
// from refresh/images.h, the headless tests from here
#define MAX_RIMAGES NUM_GLOBAL_TEXTUES
#endif
#include "../vkpt/shader/global_textures.h"
#include "../vkpt/shader/vertex_buffer.h"
#include "../vkpt/shader/global_ubo.h"


d3d12_set_layout_t d3d12_set_layouts[D3D12_NUM_SET_LAYOUTS];
d3d12_pipeline_layout_t d3d12_pipeline_layouts[D3D12_NUM_PIPELINE_LAYOUTS];

void (*d3d12_bindings_print)(const char *text);

static void report(const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (d3d12_bindings_print)
        d3d12_bindings_print(text);
    else
        fprintf(stderr, "d3d12 bindings: %s\n", text);
}

/* ------------------------------------------------------------------------- */
/* SET LAYOUTS                                                                */
/* ------------------------------------------------------------------------- */

static void add(d3d12_set_layout_t *s, unsigned binding, d3d12_bind_kind_t kind, uint32_t count)
{
    if (s->num_bindings >= D3D12_MAX_SET_BINDINGS) {
        // the table is a fixed array: a set this big is a new kind of set
        report("set %s has more than %d bindings", s->name, D3D12_MAX_SET_BINDINGS);
        return;
    }
    d3d12_binding_t *b = &s->bindings[s->num_bindings++];
    memset(b, 0, sizeof(*b));
    b->binding = (uint16_t)binding;
    b->kind = (uint8_t)kind;
    b->count = count;
}

/* A runtime-sized array: `reserve` slots of the instance's block are kept for
   it (0 = none, the caller points its table somewhere else - the global
   textures do, at the bindless block every set shares). */
static void add_unbounded(d3d12_set_layout_t *s, unsigned binding, d3d12_bind_kind_t kind, uint32_t reserve)
{
    add(s, binding, kind, reserve);
    s->bindings[s->num_bindings - 1].unbounded = true;
}

/* Lay the bindings out: resources in binding order, then each unbounded
   array's reserved slots, samplers in their own block. */
static void finish(d3d12_set_layout_t *s)
{
    uint32_t res = 0, smp = 0;
    for (int i = 0; i < s->num_bindings; i++) {
        d3d12_binding_t *b = &s->bindings[i];
        if (b->unbounded)
            continue;
        switch (b->kind) {
        case D3D12_BIND_SAMPLERS:
            b->offset = smp;
            smp += b->count;
            break;
        case D3D12_BIND_SRV_IMAGE:
            b->offset = res;
            res += b->count;
            b->sampler_offset = smp;
            smp += b->count;
            break;
        default:
            b->offset = res;
            res += b->count;
            break;
        }
    }
    for (int i = 0; i < s->num_bindings; i++) {
        d3d12_binding_t *b = &s->bindings[i];
        if (b->unbounded) {
            b->offset = res;
            res += b->count;
        }
    }
    s->num_resources = res;
    s->num_samplers = smp;
}

static void build_textures(d3d12_set_layout_t *s)
{
    s->name = "textures";
    // texture2D global_textures_d3d12[] - the bindless block, shared by all
    add_unbounded(s, GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX, D3D12_BIND_SRV_IMAGE_SEPARATE, 0);

#define IMG_DO(_name, _binding, ...) add(s, BINDING_OFFSET_IMAGES + (_binding), D3D12_BIND_UAV_IMAGE, 1);
    LIST_IMAGES
    LIST_IMAGES_A_B
#undef IMG_DO
#define IMG_DO(_name, _binding, ...) add(s, BINDING_OFFSET_TEXTURES + (_binding), D3D12_BIND_SRV_IMAGE, 1);
    LIST_IMAGES
    LIST_IMAGES_A_B
#undef IMG_DO

    add(s, BINDING_OFFSET_BLUE_NOISE, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_ENVMAP, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_PHYSICAL_SKY, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_PHYSICAL_SKY_IMG, D3D12_BIND_UAV_IMAGE, 1);
    add(s, BINDING_OFFSET_SKY_TRANSMITTANCE, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_SKY_SCATTERING, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_SKY_IRRADIANCE, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_SKY_CLOUDS, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_TERRAIN_ALBEDO, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_TERRAIN_NORMALS, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_TERRAIN_DEPTH, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_TERRAIN_SHADOWMAP, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_FLOW_DENSITY, D3D12_BIND_SRV_IMAGE, 1);
    add(s, BINDING_OFFSET_FLOW_TABLE, D3D12_BIND_UAV_BUFFER, 1);
    add(s, BINDING_OFFSET_FLOW_PARAMS, D3D12_BIND_CBV, 1);
    add(s, BINDING_OFFSET_FLOW_LIGHTS, D3D12_BIND_UAV_BUFFER, 1);
    add(s, BINDING_OFFSET_FLOW_OCCUPANCY, D3D12_BIND_UAV_BUFFER, 1);
    add(s, BINDING_OFFSET_FLOW_AMBIENT, D3D12_BIND_SRV_IMAGE, 1);

    add(s, D3D12_BINDING_GLOBAL_SAMPLERS, D3D12_BIND_SAMPLERS, D3D12_NUM_GLOBAL_SAMPLERS);
    add(s, D3D12_BINDING_GLOBAL_SAMPLER_INDEX, D3D12_BIND_UAV_BUFFER, 1);
}

static void build_ubo(d3d12_set_layout_t *s)
{
    s->name = "ubo";
    add(s, GLOBAL_UBO_BINDING_IDX, D3D12_BIND_CBV, 1);
    add(s, GLOBAL_INSTANCE_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
}

static void build_vertex_buffer(d3d12_set_layout_t *s)
{
    s->name = "vertex buffer";
    add_unbounded(s, PRIMITIVE_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, VERTEX_BUFFER_FIRST_MODEL + D3D12_MAX_MODELS);
    add(s, POSITION_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
    add(s, LIGHT_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
    add(s, LIGHT_COUNTS_HISTORY_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, LIGHT_COUNT_HISTORY);
    add(s, IQM_MATRIX_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
    add(s, READBACK_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
    add(s, TONE_MAPPING_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
    add(s, SUN_COLOR_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
    add(s, SUN_COLOR_UBO_BINDING_IDX, D3D12_BIND_CBV, 1);
    add(s, LIGHT_STATS_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 3);
    add(s, SHARC_KEY_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
    add(s, SHARC_ACCUM_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
    add(s, SHARC_RESOLVED_BUFFER_BINDING_IDX, D3D12_BIND_UAV_BUFFER, 1);
}

static void build_rt(d3d12_set_layout_t *s)
{
    s->name = "rt";
    add(s, RT_BINDING_TLAS, D3D12_BIND_SRV_AS, TLAS_COUNT);
    add(s, RT_BINDING_PARTICLE_COLOR, D3D12_BIND_SRV_TEXEL_BUFFER, 1);
    add(s, RT_BINDING_BEAM_COLOR, D3D12_BIND_SRV_TEXEL_BUFFER, 1);
    add(s, RT_BINDING_SPRITE_INFO, D3D12_BIND_SRV_TEXEL_BUFFER, 1);
    add(s, RT_BINDING_BEAM_INTERSECT, D3D12_BIND_SRV_TEXEL_BUFFER, 1);
}

static void build_pt_tiles(d3d12_set_layout_t *s)
{
    s->name = "pt tiles";
    add(s, 0, D3D12_BIND_UAV_BUFFER, 1);
}

/* god_rays.c's set: the shadow map, the fog's own TLAS, its images and
   reservoir buffer (bindings[] in god_rays.c - keep in step). */
static void build_god_rays(d3d12_set_layout_t *s)
{
    s->name = "god rays";
    add(s, 0, D3D12_BIND_SRV_IMAGE, 1);
    add(s, 1, D3D12_BIND_SRV_AS, 1);
    add(s, 2, D3D12_BIND_UAV_IMAGE, 1);
    add(s, 3, D3D12_BIND_SRV_IMAGE, 1);
    add(s, 4, D3D12_BIND_UAV_IMAGE, 1);
    add(s, 5, D3D12_BIND_SRV_IMAGE, 1);
    add(s, 6, D3D12_BIND_UAV_IMAGE, 1);
    add(s, 7, D3D12_BIND_UAV_IMAGE, 1);
    add(s, 8, D3D12_BIND_UAV_IMAGE, 1);
    add(s, 9, D3D12_BIND_SRV_IMAGE, 1);
    add(s, 10, D3D12_BIND_UAV_IMAGE, 1);
    add(s, 11, D3D12_BIND_UAV_BUFFER, 1);
}

/* flow.c's env set: the TLAS its probes trace, and two images. */
static void build_flow_env(d3d12_set_layout_t *s)
{
    s->name = "flow env";
    add(s, 0, D3D12_BIND_SRV_AS, 1);
    add(s, 1, D3D12_BIND_UAV_IMAGE, 1);
    add(s, 2, D3D12_BIND_UAV_IMAGE, 1);
}

/* sound_trace.c's set: the sound TLAS, the trace parameters, and the
   triangle, field, edge and diffraction buffers. */
static void build_sound(d3d12_set_layout_t *s)
{
    s->name = "sound";
    add(s, 0, D3D12_BIND_SRV_AS, 1);
    add(s, 1, D3D12_BIND_CBV, 1);
    add(s, 2, D3D12_BIND_UAV_BUFFER, 1);
    add(s, 3, D3D12_BIND_UAV_BUFFER, 1);
    add(s, 4, D3D12_BIND_UAV_BUFFER, 1);
    add(s, 5, D3D12_BIND_UAV_BUFFER, 1);
}

/* textures.c's normal map pass: every texture as a storage image. Its table is
   pointed at a block the caller makes, so nothing is reserved here. */
static void build_normalize(d3d12_set_layout_t *s)
{
    s->name = "normalize";
    add_unbounded(s, 0, D3D12_BIND_UAV_IMAGE, 0);
}

/* precomputed_sky.c's set: the atmosphere parameters (precomputed_sky.glsl's
   PRECOMPUTED_SKY_UBO_DESC_SET_IDX 3 / PRECOMPUTED_SKY_BINDING_IDX 0, GLSL-only
   macros - keep in step). */
#define PRECOMPUTED_SKY_SET     3
static void build_sky(d3d12_set_layout_t *s)
{
    s->name = "sky";
    add(s, 0, D3D12_BIND_CBV, 1);
}

static void pipeline(d3d12_pipeline_layout_id_t id, const char *name, int n, const int *layouts, const int *indices)
{
    d3d12_pipeline_layout_t *pl = &d3d12_pipeline_layouts[id];
    pl->name = name;
    pl->num_sets = n;
    for (int i = 0; i < n; i++) {
        pl->set_layout[i] = layouts[i];
        pl->set_index[i] = indices[i];
    }
}

void d3d12_bindings_build_layouts(void)
{
    static bool built;
    if (built)
        return;
    built = true;

    static void (*const builders[D3D12_NUM_SET_LAYOUTS])(d3d12_set_layout_t *) = {
        [D3D12_SET_TEXTURES]        = build_textures,
        [D3D12_SET_UBO]             = build_ubo,
        [D3D12_SET_VERTEX_BUFFER]   = build_vertex_buffer,
        [D3D12_SET_RT]              = build_rt,
        [D3D12_SET_PT_TILES]        = build_pt_tiles,
        [D3D12_SET_GOD_RAYS]        = build_god_rays,
        [D3D12_SET_FLOW_ENV]        = build_flow_env,
        [D3D12_SET_SOUND]           = build_sound,
        [D3D12_SET_NORMALIZE]       = build_normalize,
        [D3D12_SET_SKY]             = build_sky,
    };
    for (int i = 0; i < D3D12_NUM_SET_LAYOUTS; i++) {
        memset(&d3d12_set_layouts[i], 0, sizeof(d3d12_set_layouts[i]));
        builders[i](&d3d12_set_layouts[i]);
        finish(&d3d12_set_layouts[i]);
    }

    // the set lists of vkpt's pipeline layouts, in their Vulkan set order
    {
        const int l[] = { D3D12_SET_RT, D3D12_SET_UBO, D3D12_SET_TEXTURES, D3D12_SET_VERTEX_BUFFER, D3D12_SET_PT_TILES };
        const int x[] = { 0, 1, 2, 3, D3D12_PT_TILE_DESC_SET };
        pipeline(D3D12_PL_PATH_TRACER, "path tracer", 5, l, x);
    }
    {
        const int l[] = { D3D12_SET_UBO, D3D12_SET_TEXTURES, D3D12_SET_VERTEX_BUFFER };
        const int x[] = { 0, 1, 2 };
        pipeline(D3D12_PL_STANDARD, "standard", 3, l, x);
    }
    {
        const int l[] = { D3D12_SET_UBO, D3D12_SET_TEXTURES };
        const int x[] = { 0, 1 };
        pipeline(D3D12_PL_UBO_TEXTURES, "ubo+textures", 2, l, x);
    }
    {
        const int l[] = { D3D12_SET_UBO, D3D12_SET_VERTEX_BUFFER };
        const int x[] = { 0, 1 };
        pipeline(D3D12_PL_UBO_VERTEX_BUFFER, "ubo+vertex buffer", 2, l, x);
    }
    {
        const int l[] = { D3D12_SET_GOD_RAYS, D3D12_SET_VERTEX_BUFFER, D3D12_SET_UBO, D3D12_SET_TEXTURES };
        const int x[] = { 0, 1, 2, 3 };
        pipeline(D3D12_PL_GOD_RAYS, "god rays", 4, l, x);
    }
    {
        const int l[] = { D3D12_SET_FLOW_ENV, D3D12_SET_VERTEX_BUFFER, D3D12_SET_UBO, D3D12_SET_TEXTURES };
        const int x[] = { 0, 1, 2, 3 };
        pipeline(D3D12_PL_FLOW_ENV, "flow env", 4, l, x);
    }
    {
        const int l[] = { D3D12_SET_SOUND };
        const int x[] = { 0 };
        pipeline(D3D12_PL_SOUND, "sound", 1, l, x);
    }
    {
        const int l[] = { D3D12_SET_NORMALIZE };
        const int x[] = { 0 };
        pipeline(D3D12_PL_NORMALIZE, "normalize", 1, l, x);
    }
    {
        const int l[] = { D3D12_SET_UBO, D3D12_SET_TEXTURES, D3D12_SET_VERTEX_BUFFER, D3D12_SET_SKY };
        const int x[] = { 0, 1, 2, PRECOMPUTED_SKY_SET };
        pipeline(D3D12_PL_PHYSICAL_SKY, "physical sky", 4, l, x);
    }
    // the sky's terrain shadow map: shadow_map.vert reads a vertex attribute
    // and its push constants, nothing else
    pipeline(D3D12_PL_SHADOW_MAP, "shadow map", 0, NULL, NULL);
    d3d12_pipeline_layouts[D3D12_PL_SHADOW_MAP].input_assembler = true;
}

const d3d12_binding_t *d3d12_set_find_binding(const d3d12_set_layout_t *layout, unsigned binding)
{
    for (int i = 0; i < layout->num_bindings; i++)
        if (layout->bindings[i].binding == binding)
            return &layout->bindings[i];
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* ROOT SIGNATURES                                                            */
/* ------------------------------------------------------------------------- */

static D3D12_DESCRIPTOR_RANGE_TYPE range_type(d3d12_bind_kind_t kind)
{
    switch (kind) {
    case D3D12_BIND_CBV:            return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    case D3D12_BIND_UAV_BUFFER:
    case D3D12_BIND_UAV_IMAGE:      return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    case D3D12_BIND_SAMPLERS:       return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    default:                        return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    }
}

/* Vulkan's update-after-bind semantics: a descriptor may be rewritten after a
   list that reads it is recorded (but before it runs), and an image written by
   one dispatch is read by the next while the same table stays bound - so
   nothing may be assumed static. */
#define RANGE_FLAGS     (D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE)
#define SAMPLER_FLAGS   D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE

#define MAX_RANGES      (D3D12_MAX_PIPELINE_SETS * D3D12_MAX_SET_BINDINGS * 2)
#define MAX_PARAMS      (D3D12_MAX_PIPELINE_SETS * (2 + D3D12_MAX_UNBOUNDED_PER_SET) + 2)

static HRESULT create_root_signature(ID3D12Device *device, PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE serialize,
                                     d3d12_pipeline_layout_t *pl, char *error, size_t error_size)
{
    static D3D12_DESCRIPTOR_RANGE1 ranges[MAX_RANGES];
    D3D12_ROOT_PARAMETER1 params[MAX_PARAMS];
    int num_ranges = 0, num_params = 0;

    memset(params, 0, sizeof(params));

    for (int si = 0; si < pl->num_sets; si++) {
        const d3d12_set_layout_t *s = &d3d12_set_layouts[pl->set_layout[si]];
        const unsigned set = (unsigned)pl->set_index[si];
        int unbounded = 0;

        pl->param_resources[si] = -1;
        pl->param_samplers[si] = -1;
        for (int u = 0; u < D3D12_MAX_UNBOUNDED_PER_SET; u++)
            pl->param_unbounded[si][u] = -1;

        // the resource table
        int first = num_ranges;
        for (int i = 0; i < s->num_bindings; i++) {
            const d3d12_binding_t *b = &s->bindings[i];
            if (b->unbounded || b->kind == D3D12_BIND_SAMPLERS)
                continue;
            D3D12_DESCRIPTOR_RANGE1 *r = &ranges[num_ranges++];
            r->RangeType = range_type((d3d12_bind_kind_t)b->kind);
            r->NumDescriptors = b->count;
            r->BaseShaderRegister = 0;
            r->RegisterSpace = D3D12_SPACE(set, b->binding);
            // CBV ranges take DATA_VOLATILE too; only samplers cannot
            r->Flags = RANGE_FLAGS;
            r->OffsetInDescriptorsFromTableStart = b->offset;
        }
        if (num_ranges > first) {
            D3D12_ROOT_PARAMETER1 *p = &params[num_params];
            p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p->DescriptorTable.NumDescriptorRanges = (UINT)(num_ranges - first);
            p->DescriptorTable.pDescriptorRanges = &ranges[first];
            p->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            pl->param_resources[si] = (int8_t)num_params++;
        }

        // the sampler table: combined image samplers' halves and plain samplers
        first = num_ranges;
        for (int i = 0; i < s->num_bindings; i++) {
            const d3d12_binding_t *b = &s->bindings[i];
            if (b->unbounded || (b->kind != D3D12_BIND_SAMPLERS && b->kind != D3D12_BIND_SRV_IMAGE))
                continue;
            D3D12_DESCRIPTOR_RANGE1 *r = &ranges[num_ranges++];
            r->RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            r->NumDescriptors = b->count;
            r->BaseShaderRegister = 0;
            r->RegisterSpace = D3D12_SPACE(set, b->binding);
            r->Flags = SAMPLER_FLAGS;
            r->OffsetInDescriptorsFromTableStart = b->kind == D3D12_BIND_SRV_IMAGE ? b->sampler_offset : b->offset;
        }
        if (num_ranges > first) {
            D3D12_ROOT_PARAMETER1 *p = &params[num_params];
            p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p->DescriptorTable.NumDescriptorRanges = (UINT)(num_ranges - first);
            p->DescriptorTable.pDescriptorRanges = &ranges[first];
            p->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            pl->param_samplers[si] = (int8_t)num_params++;
        }

        // every unbounded array, a table of its own
        for (int i = 0; i < s->num_bindings; i++) {
            const d3d12_binding_t *b = &s->bindings[i];
            if (!b->unbounded)
                continue;
            if (unbounded >= D3D12_MAX_UNBOUNDED_PER_SET) {
                snprintf(error, error_size, "%s: set %s has more than %d unbounded arrays", pl->name, s->name,
                         D3D12_MAX_UNBOUNDED_PER_SET);
                return E_INVALIDARG;
            }
            D3D12_DESCRIPTOR_RANGE1 *r = &ranges[num_ranges++];
            r->RangeType = range_type((d3d12_bind_kind_t)b->kind);
            r->NumDescriptors = UINT_MAX;
            r->BaseShaderRegister = 0;
            r->RegisterSpace = D3D12_SPACE(set, b->binding);
            r->Flags = RANGE_FLAGS;
            r->OffsetInDescriptorsFromTableStart = 0;
            D3D12_ROOT_PARAMETER1 *p = &params[num_params];
            p->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p->DescriptorTable.NumDescriptorRanges = 1;
            p->DescriptorTable.pDescriptorRanges = r;
            p->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            pl->param_unbounded[si][unbounded++] = (int8_t)num_params++;
        }
    }

    // push constants, then gl_NumWorkGroups
    params[num_params].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[num_params].Constants.ShaderRegister = 0;
    params[num_params].Constants.RegisterSpace = D3D12_SPACE_PUSH_CONSTANTS;
    params[num_params].Constants.Num32BitValues = D3D12_PUSH_CONSTANT_DWORDS;
    params[num_params].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    pl->param_push_constants = (int8_t)num_params++;

    params[num_params].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[num_params].Constants.ShaderRegister = 0;
    params[num_params].Constants.RegisterSpace = D3D12_SPACE_NUM_WORKGROUPS;
    params[num_params].Constants.Num32BitValues = 3;
    params[num_params].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    pl->param_num_workgroups = (int8_t)num_params++;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = (UINT)num_params;
    desc.Desc_1_1.pParameters = params;
    desc.Desc_1_1.Flags = pl->input_assembler ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                                              : D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = serialize(&desc, &blob, &err);
    if (FAILED(hr)) {
        snprintf(error, error_size, "%s: root signature: %s", pl->name,
                 err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "serialization failed");
        if (err)
            ID3D10Blob_Release(err);
        return hr;
    }
    if (err)
        ID3D10Blob_Release(err);

    hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                          &IID_ID3D12RootSignature, (void **)&pl->root_signature);
    ID3D10Blob_Release(blob);
    if (FAILED(hr))
        snprintf(error, error_size, "%s: CreateRootSignature 0x%08lx", pl->name, (unsigned long)hr);
    return hr;
}

HRESULT d3d12_bindings_create_root_signatures(ID3D12Device *device,
                                              PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE serialize,
                                              char *error, size_t error_size)
{
    d3d12_bindings_build_layouts();
    for (int i = 0; i < D3D12_NUM_PIPELINE_LAYOUTS; i++) {
        HRESULT hr = create_root_signature(device, serialize, &d3d12_pipeline_layouts[i], error, error_size);
        if (FAILED(hr)) {
            d3d12_bindings_destroy_root_signatures();
            return hr;
        }
    }
    return S_OK;
}

void d3d12_bindings_destroy_root_signatures(void)
{
    for (int i = 0; i < D3D12_NUM_PIPELINE_LAYOUTS; i++) {
        if (d3d12_pipeline_layouts[i].root_signature) {
            ID3D12RootSignature_Release(d3d12_pipeline_layouts[i].root_signature);
            d3d12_pipeline_layouts[i].root_signature = NULL;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* THE G-BUFFER IMAGES                                                        */
/* ------------------------------------------------------------------------- */

/* LIST_IMAGES names Vulkan formats; these are the DXGI ones. A format added to
   the list without a line here fails to compile, not at run time. */
#define D3D12_VKFMT_R8G8_UNORM              DXGI_FORMAT_R8G8_UNORM
#define D3D12_VKFMT_R8G8B8A8_UNORM          DXGI_FORMAT_R8G8B8A8_UNORM
#define D3D12_VKFMT_R16_SFLOAT              DXGI_FORMAT_R16_FLOAT
#define D3D12_VKFMT_R16_UINT                DXGI_FORMAT_R16_UINT
#define D3D12_VKFMT_R16G16_SFLOAT           DXGI_FORMAT_R16G16_FLOAT
#define D3D12_VKFMT_R16G16B16A16_SFLOAT     DXGI_FORMAT_R16G16B16A16_FLOAT
#define D3D12_VKFMT_R32_SFLOAT              DXGI_FORMAT_R32_FLOAT
#define D3D12_VKFMT_R32_UINT                DXGI_FORMAT_R32_UINT
#define D3D12_VKFMT_R32G32_SFLOAT           DXGI_FORMAT_R32G32_FLOAT
#define D3D12_VKFMT_R32G32_UINT             DXGI_FORMAT_R32G32_UINT
#define D3D12_VKFMT_R32G32B32A32_SFLOAT     DXGI_FORMAT_R32G32B32A32_FLOAT
#define D3D12_VKFMT_R32G32B32A32_UINT       DXGI_FORMAT_R32G32B32A32_UINT

static const struct {
    const char  *name;
    DXGI_FORMAT format;
    unsigned    binding;
} images[NUM_VKPT_IMAGES] = {
#define IMG_DO(_name, _binding, _vkformat, ...) [VKPT_IMG_##_name] = { #_name, D3D12_VKFMT_##_vkformat, _binding },
    LIST_IMAGES
    LIST_IMAGES_A_B
#undef IMG_DO
};

DXGI_FORMAT d3d12_image_format(int image)
{
    if (image < 0 || image >= NUM_VKPT_IMAGES)
        return DXGI_FORMAT_UNKNOWN;
    return images[image].format;
}

int d3d12_num_images(void)
{
    return NUM_VKPT_IMAGES;
}

const char *d3d12_image_name(int image)
{
    if (image < 0 || image >= NUM_VKPT_IMAGES)
        return "?";
    return images[image].name;
}

unsigned d3d12_image_binding(int image)
{
    return (image >= 0 && image < NUM_VKPT_IMAGES) ? images[image].binding : 0;
}

/* The sizes: LIST_IMAGES's own expressions, over d3d12_extents_t instead of
   qvk's extents. */
#undef IMG_WIDTH
#undef IMG_HEIGHT
#undef IMG_WIDTH_MGPU
#undef IMG_WIDTH_UNSCALED
#undef IMG_HEIGHT_UNSCALED
#undef IMG_WIDTH_GRAD
#undef IMG_HEIGHT_GRAD
#undef IMG_WIDTH_GRAD_MGPU
#undef IMG_WIDTH_TAA
#undef IMG_HEIGHT_TAA
#undef IMG_WIDTH_POSTFX
#undef IMG_HEIGHT_POSTFX
#define IMG_WIDTH               (e->screen_width)
#define IMG_HEIGHT              (e->screen_height)
#define IMG_WIDTH_MGPU          IMG_WIDTH
#define IMG_WIDTH_UNSCALED      (e->unscaled_width)
#define IMG_HEIGHT_UNSCALED     (e->unscaled_height)
#define IMG_WIDTH_GRAD          ((e->screen_width + GRAD_DWN - 1) / GRAD_DWN)
#define IMG_HEIGHT_GRAD         ((e->screen_height + GRAD_DWN - 1) / GRAD_DWN)
#define IMG_WIDTH_GRAD_MGPU     IMG_WIDTH_GRAD
#define IMG_WIDTH_TAA           (e->taa_width)
#define IMG_HEIGHT_TAA          (e->taa_height)
#define IMG_WIDTH_POSTFX        (e->taa_width > e->unscaled_width ? e->taa_width : e->unscaled_width)
#define IMG_HEIGHT_POSTFX       (e->taa_height > e->unscaled_height ? e->taa_height : e->unscaled_height)

void d3d12_image_extent(int image, const d3d12_extents_t *e, uint32_t *width, uint32_t *height)
{
    uint32_t w = 0, h = 0;
    switch (image) {
#define IMG_DO(_name, _binding, _vkformat, _glslformat, _w, _h) \
    case VKPT_IMG_##_name: w = (uint32_t)(_w); h = (uint32_t)(_h); break;
    LIST_IMAGES
    LIST_IMAGES_A_B
#undef IMG_DO
    default: break;
    }
    // vkpt never makes a 0-sized image either (an extent of 0 is "not in use")
    *width = w ? w : 1;
    *height = h ? h : 1;
}

int d3d12_image_at_binding(int image, int odd)
{
    if (!odd || image < 0 || image >= NUM_VKPT_IMAGES)
        return image;
    const unsigned binding = images[image].binding;
#define IMG_DO(_name, _binding, ...) if ((_binding) == binding) return VKPT_IMG_##_name;
    LIST_IMAGES_B_A
#undef IMG_DO
    return image;   // not an _A/_B image: the same on both
}

int d3d12_image_sampler(int image)
{
    // vkpt textures.c: nearest unless one of these
    if ((image >= VKPT_IMG_BLOOM_HBLUR && image <= VKPT_IMG_BLOOM_VBLUR) ||
        (image >= VKPT_IMG_DLSS_BLOOM_HBLUR && image <= VKPT_IMG_DLSS_BLOOM_VBLUR))
        return D3D12_GLOBAL_SAMPLER_LINEAR_CLAMP;
    if (image == VKPT_IMG_ASVGF_TAA_A || image == VKPT_IMG_ASVGF_TAA_B ||
        image == VKPT_IMG_TAA_OUTPUT || image == VKPT_IMG_DLSS_OUTPUT)
        return D3D12_GLOBAL_SAMPLER_DEFAULT;
    return D3D12_GLOBAL_SAMPLER_NEAREST;
}

/* ------------------------------------------------------------------------- */
/* SAMPLERS                                                                   */
/* ------------------------------------------------------------------------- */

/* The same settings as vkpt's (textures.c, vkpt_textures_initialize). Two
   differences D3D12 forces: Vulkan's "nearest magnification + anisotropic
   minification" has no D3D12 filter, so NEAREST_MIPMAP_ANISO is point mag /
   linear min and mips without anisotropy; and vkpt's nearest and linear-clamp
   samplers leave maxLod at 0, so they read mip 0 only - kept here as MaxLOD 0. */
void d3d12_global_sampler_desc(int kind, D3D12_SAMPLER_DESC *s)
{
    memset(s, 0, sizeof(*s));
    s->AddressU = s->AddressV = s->AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s->MaxAnisotropy = 1;
    s->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s->BorderColor[3] = 1.f;
    s->MinLOD = 0.f;
    s->MaxLOD = D3D12_FLOAT32_MAX;

    switch (kind) {
    default:
    case D3D12_GLOBAL_SAMPLER_DEFAULT:
        s->Filter = D3D12_FILTER_ANISOTROPIC;
        s->MaxAnisotropy = 16;
        break;
    case D3D12_GLOBAL_SAMPLER_NEAREST:
        s->Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        s->MaxLOD = 0.f;
        break;
    case D3D12_GLOBAL_SAMPLER_NEAREST_MIPMAP_ANISO:
        s->Filter = D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
        break;
    case D3D12_GLOBAL_SAMPLER_LINEAR_CLAMP:
        s->Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s->AddressU = s->AddressV = s->AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s->MaxLOD = 0.f;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* SET INSTANCES                                                              */
/* ------------------------------------------------------------------------- */

d3d12_descriptor_heaps_t d3d12_heaps;

static D3D12_CPU_DESCRIPTOR_HANDLE resource_cpu(uint32_t index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = d3d12_heaps.resource_cpu;
    h.ptr += (SIZE_T)index * d3d12_heaps.resource_increment;
    return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE resource_gpu(uint32_t index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = d3d12_heaps.resource_gpu;
    h.ptr += (UINT64)index * d3d12_heaps.resource_increment;
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu(uint32_t index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = d3d12_heaps.sampler_cpu;
    h.ptr += (SIZE_T)index * d3d12_heaps.sampler_increment;
    return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu(uint32_t index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = d3d12_heaps.sampler_gpu;
    h.ptr += (UINT64)index * d3d12_heaps.sampler_increment;
    return h;
}

/* Null descriptors of each kind. Their dimension only has to be one the kind
   allows: a null view reads zeros (and drops writes) whatever the shader
   declared. */
static void write_null(const d3d12_binding_t *b, D3D12_CPU_DESCRIPTOR_HANDLE res, D3D12_CPU_DESCRIPTOR_HANDLE smp)
{
    ID3D12Device *dev = d3d12_heaps.device;
    switch (b->kind) {
    case D3D12_BIND_CBV: {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cv = { 0 };
        ID3D12Device_CreateConstantBufferView(dev, &cv, res);
        break;
    }
    case D3D12_BIND_UAV_BUFFER: {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = { 0 };
        uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = 1;
        uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        ID3D12Device_CreateUnorderedAccessView(dev, NULL, NULL, &uv, res);
        break;
    }
    case D3D12_BIND_UAV_IMAGE: {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = { 0 };
        uv.Format = DXGI_FORMAT_R32_FLOAT;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ID3D12Device_CreateUnorderedAccessView(dev, NULL, NULL, &uv, res);
        break;
    }
    case D3D12_BIND_SRV_IMAGE:
    case D3D12_BIND_SRV_IMAGE_SEPARATE: {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(dev, NULL, &sv, res);
        break;
    }
    case D3D12_BIND_SRV_TEXEL_BUFFER: {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.NumElements = 1;
        ID3D12Device_CreateShaderResourceView(dev, NULL, &sv, res);
        break;
    }
    case D3D12_BIND_SRV_AS: {
        // a null acceleration structure: every ray misses
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
        sv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.RaytracingAccelerationStructure.Location = 0;
        ID3D12Device_CreateShaderResourceView(dev, NULL, &sv, res);
        break;
    }
    default:
        break;
    }

    if (b->kind == D3D12_BIND_SRV_IMAGE || b->kind == D3D12_BIND_SAMPLERS) {
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(D3D12_GLOBAL_SAMPLER_NEAREST, &sd);
        ID3D12Device_CreateSampler(dev, &sd, smp);
    }
}

bool d3d12_set_alloc(d3d12_set_t *set, d3d12_set_layout_id_t layout, const char *what)
{
    d3d12_bindings_build_layouts();
    const d3d12_set_layout_t *s = &d3d12_set_layouts[layout];

    memset(set, 0, sizeof(*set));
    set->layout = -1;
    if (d3d12_heaps.resource_next + s->num_resources > d3d12_heaps.resource_end ||
        d3d12_heaps.sampler_next + s->num_samplers > d3d12_heaps.sampler_end) {
        report("no room for a %s set (%s)", s->name, what ? what : "?");
        return false;
    }

    set->layout = (int)layout;
    set->resource_base = d3d12_heaps.resource_next;
    set->sampler_base = d3d12_heaps.sampler_next;
    d3d12_heaps.resource_next += s->num_resources;
    d3d12_heaps.sampler_next += s->num_samplers;

    for (int i = 0; i < s->num_bindings; i++) {
        const d3d12_binding_t *b = &s->bindings[i];
        for (uint32_t e = 0; e < b->count; e++) {
            uint32_t r = set->resource_base + b->offset + e;
            uint32_t sm = set->sampler_base + (b->kind == D3D12_BIND_SAMPLERS ? b->offset : b->sampler_offset) + e;
            write_null(b, resource_cpu(r), sampler_cpu(sm));
        }
    }
    return true;
}

/* The descriptor slot of (binding, element), or false (and a message) when the
   set has no such binding or it is not of the kind being written. */
static bool slot(const d3d12_set_t *set, unsigned binding, unsigned element, d3d12_bind_kind_t kind,
                 const d3d12_binding_t **out)
{
    if (!set || set->layout < 0)
        return false;
    const d3d12_set_layout_t *s = &d3d12_set_layouts[set->layout];
    const d3d12_binding_t *b = d3d12_set_find_binding(s, binding);
    if (!b || b->kind != (uint8_t)kind || (b->count && element >= b->count) || (b->unbounded && !b->count)) {
        report("set %s has no %s binding %u[%u]", s->name,
                b ? "matching" : "such", binding, element);
        return false;
    }
    *out = b;
    return true;
}

void d3d12_set_write_cbv(const d3d12_set_t *set, unsigned binding, unsigned element,
                         D3D12_GPU_VIRTUAL_ADDRESS address, uint32_t size)
{
    const d3d12_binding_t *b;
    if (!slot(set, binding, element, D3D12_BIND_CBV, &b))
        return;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv;
    cv.BufferLocation = address;
    cv.SizeInBytes = (size + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
                     ~(uint32_t)(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);
    ID3D12Device_CreateConstantBufferView(d3d12_heaps.device, &cv, resource_cpu(set->resource_base + b->offset + element));
}

void d3d12_set_write_buffer(const d3d12_set_t *set, unsigned binding, unsigned element,
                            ID3D12Resource *buffer, uint64_t offset, uint64_t size)
{
    const d3d12_binding_t *b;
    if (!slot(set, binding, element, D3D12_BIND_UAV_BUFFER, &b))
        return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = { 0 };
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Buffer.FirstElement = offset / 4;
    uv.Buffer.NumElements = (UINT)((size + 3) / 4);
    uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(d3d12_heaps.device, buffer, NULL, &uv,
                                           resource_cpu(set->resource_base + b->offset + element));
}

void d3d12_set_write_uav_image(const d3d12_set_t *set, unsigned binding, unsigned element,
                               ID3D12Resource *image, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    const d3d12_binding_t *b;
    if (!slot(set, binding, element, D3D12_BIND_UAV_IMAGE, &b))
        return;
    ID3D12Device_CreateUnorderedAccessView(d3d12_heaps.device, image, NULL, desc,
                                           resource_cpu(set->resource_base + b->offset + element));
}

void d3d12_set_write_srv_image(const d3d12_set_t *set, unsigned binding, unsigned element,
                               ID3D12Resource *image, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                               const D3D12_SAMPLER_DESC *sampler)
{
    const d3d12_binding_t *b;
    if (!slot(set, binding, element, D3D12_BIND_SRV_IMAGE, &b))
        return;
    ID3D12Device_CreateShaderResourceView(d3d12_heaps.device, image, desc,
                                          resource_cpu(set->resource_base + b->offset + element));
    if (sampler)
        ID3D12Device_CreateSampler(d3d12_heaps.device, sampler, sampler_cpu(set->sampler_base + b->sampler_offset + element));
}

void d3d12_set_write_texel_buffer(const d3d12_set_t *set, unsigned binding, unsigned element,
                                  ID3D12Resource *buffer, DXGI_FORMAT format, uint64_t first, uint32_t count)
{
    const d3d12_binding_t *b;
    if (!slot(set, binding, element, D3D12_BIND_SRV_TEXEL_BUFFER, &b))
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Buffer.FirstElement = first;
    sv.Buffer.NumElements = count;
    ID3D12Device_CreateShaderResourceView(d3d12_heaps.device, buffer, &sv,
                                          resource_cpu(set->resource_base + b->offset + element));
}

void d3d12_set_write_as(const d3d12_set_t *set, unsigned binding, unsigned element, D3D12_GPU_VIRTUAL_ADDRESS tlas)
{
    const d3d12_binding_t *b;
    if (!slot(set, binding, element, D3D12_BIND_SRV_AS, &b))
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.RaytracingAccelerationStructure.Location = tlas;
    ID3D12Device_CreateShaderResourceView(d3d12_heaps.device, NULL, &sv,
                                          resource_cpu(set->resource_base + b->offset + element));
}

void d3d12_set_write_sampler(const d3d12_set_t *set, unsigned binding, unsigned element,
                             const D3D12_SAMPLER_DESC *sampler)
{
    const d3d12_binding_t *b;
    if (!slot(set, binding, element, D3D12_BIND_SAMPLERS, &b))
        return;
    ID3D12Device_CreateSampler(d3d12_heaps.device, sampler, sampler_cpu(set->sampler_base + b->offset + element));
}

/* ------------------------------------------------------------------------- */
/* COMPUTE                                                                    */
/* ------------------------------------------------------------------------- */

ID3D12StateObject *d3d12_create_raygen_object(d3d12_pipeline_layout_id_t layout, const void *dxil, size_t size,
                                              void *identifier, const wchar_t *name)
{
    ID3D12Device5 *dev5 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(d3d12_heaps.device, &IID_ID3D12Device5, (void **)&dev5)))
        return NULL;

    D3D12_EXPORT_DESC export_desc = { L"main", NULL, D3D12_EXPORT_FLAG_NONE };
    D3D12_DXIL_LIBRARY_DESC library = { { dxil, size }, 1, &export_desc };
    D3D12_GLOBAL_ROOT_SIGNATURE global = { d3d12_pipeline_layouts[layout].root_signature };
    // no TraceRay (the rays are RayQuery): the smallest payload, and no recursion
    D3D12_RAYTRACING_SHADER_CONFIG shader_config = { 4, 8 };
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config = { 1 };
    D3D12_STATE_SUBOBJECT subobjects[4] = {
        { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library },
        { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config },
    };
    D3D12_STATE_OBJECT_DESC desc = { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 4, subobjects };

    ID3D12StateObject *so = NULL;
    HRESULT hr = ID3D12Device5_CreateStateObject(dev5, &desc, &IID_ID3D12StateObject, (void **)&so);
    ID3D12Device5_Release(dev5);
    if (FAILED(hr))
        return NULL;

    ID3D12StateObjectProperties *props = NULL;
    const void *id = NULL;
    if (SUCCEEDED(ID3D12StateObject_QueryInterface(so, &IID_ID3D12StateObjectProperties, (void **)&props))) {
        id = ID3D12StateObjectProperties_GetShaderIdentifier(props, L"main");
        if (id)
            memcpy(identifier, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        ID3D12StateObjectProperties_Release(props);
    }
    if (!id) {
        ID3D12StateObject_Release(so);
        return NULL;
    }
    if (name)
        ID3D12StateObject_SetName(so, name);
    return so;
}

ID3D12PipelineState *d3d12_create_compute_pso(d3d12_pipeline_layout_id_t layout, const void *dxil, size_t size,
                                              const wchar_t *name)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof(pd));
    pd.pRootSignature = d3d12_pipeline_layouts[layout].root_signature;
    pd.CS.pShaderBytecode = dxil;
    pd.CS.BytecodeLength = size;
    ID3D12PipelineState *pso = NULL;
    if (FAILED(ID3D12Device_CreateComputePipelineState(d3d12_heaps.device, &pd, &IID_ID3D12PipelineState,
                                                       (void **)&pso)))
        return NULL;
    if (name)
        ID3D12PipelineState_SetName(pso, name);
    return pso;
}

void d3d12_compute_begin(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout)
{
    // the heaps are the renderer's: the caller sets them (SetDescriptorHeaps
    // takes the ID3D12DescriptorHeap objects, which this file never sees)
    ID3D12GraphicsCommandList_SetComputeRootSignature(cmd, d3d12_pipeline_layouts[layout].root_signature);
}

static void set_table(ID3D12GraphicsCommandList *cmd, bool graphics, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h)
{
    if (graphics)
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, param, h);
    else
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cmd, param, h);
}

static void bind_sets(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                      const d3d12_set_t *const *sets, bool graphics)
{
    const d3d12_pipeline_layout_t *pl = &d3d12_pipeline_layouts[layout];
    for (int si = 0; si < pl->num_sets; si++) {
        const d3d12_set_t *set = sets[si];
        if (!set || set->layout != pl->set_layout[si])
            continue;   // a wrong or missing set leaves the table unset: the debug layer names it
        const d3d12_set_layout_t *s = &d3d12_set_layouts[set->layout];

        if (pl->param_resources[si] >= 0)
            set_table(cmd, graphics, (UINT)pl->param_resources[si], resource_gpu(set->resource_base));
        if (pl->param_samplers[si] >= 0)
            set_table(cmd, graphics, (UINT)pl->param_samplers[si], sampler_gpu(set->sampler_base));
        int u = 0;
        for (int i = 0; i < s->num_bindings && u < D3D12_MAX_UNBOUNDED_PER_SET; i++) {
            const d3d12_binding_t *b = &s->bindings[i];
            if (!b->unbounded)
                continue;
            const uint32_t start = b->count ? set->resource_base + b->offset : set->external[u];
            if (pl->param_unbounded[si][u] >= 0)
                set_table(cmd, graphics, (UINT)pl->param_unbounded[si][u], resource_gpu(start));
            u++;
        }
    }
}

static void push_constants(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                           const void *data, size_t size, bool graphics)
{
    const d3d12_pipeline_layout_t *pl = &d3d12_pipeline_layouts[layout];
    UINT dwords = (UINT)((size + 3) / 4);
    if (dwords > D3D12_PUSH_CONSTANT_DWORDS)
        dwords = D3D12_PUSH_CONSTANT_DWORDS;
    if (graphics)
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cmd, (UINT)pl->param_push_constants, dwords, data, 0);
    else
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cmd, (UINT)pl->param_push_constants, dwords, data, 0);
}

void d3d12_compute_bind_sets(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                             const d3d12_set_t *const *sets)
{
    bind_sets(cmd, layout, sets, false);
}

void d3d12_compute_push_constants(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                                  const void *data, size_t size)
{
    push_constants(cmd, layout, data, size, false);
}

void d3d12_graphics_begin(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout)
{
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cmd, d3d12_pipeline_layouts[layout].root_signature);
}

void d3d12_graphics_bind_sets(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                              const d3d12_set_t *const *sets)
{
    bind_sets(cmd, layout, sets, true);
}

void d3d12_graphics_push_constants(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                                   const void *data, size_t size)
{
    push_constants(cmd, layout, data, size, true);
}

void d3d12_compute_dispatch(ID3D12GraphicsCommandList *cmd, d3d12_pipeline_layout_id_t layout,
                            uint32_t x, uint32_t y, uint32_t z)
{
    const d3d12_pipeline_layout_t *pl = &d3d12_pipeline_layouts[layout];
    const uint32_t groups[3] = { x, y, z };
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cmd, (UINT)pl->param_num_workgroups, 3, groups, 0);
    ID3D12GraphicsCommandList_Dispatch(cmd, x, y, z);
}
