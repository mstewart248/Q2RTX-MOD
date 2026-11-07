// Headless check of world.c's primary-ray pass: vkpt's real
// primary_rays.rgen.query, dispatched as world.c dispatches it, against a
// two-quad world built the way d3d12_world_load and vkpt_prepare_frame build
// theirs:
//   * a world buffer of VboPrimitive[] then the positions (9 floats each),
//     read as primitive_buffers[VERTEX_BUFFER_WORLD]
//   * one BLAS over the positions, one TLAS instance as geom_opaque's
//     (custom index VERTEX_BUFFER_WORLD, mask OPAQUE | WORLD_STATIC, FORCE_OPAQUE)
//   * the instance buffer's tlas_instance_model_indices[0] = -1 (world), as
//     vkpt_pt_instance_model_blas writes it
//   * a UBO with a camera at the origin looking down +z, checkerboard fields
//     (pt_fullres_fields 0) side by side at pt_field_offset = width / 2
// The quads, both double-wound so either facing convention sees them, cover
// the lower half of the view: x < 0 at z = 100 (material 1), x > 0 at z = 200
// (material 2). PT_SHADING_POSITION is read back and every pixel checked
// against the ray it traced: the hit point and material, or a miss (material 0).
//
// Phase 2, the lit frame: passes.c's d3d12_record_passes - the recording
// world.c does in the game - over the same world, frame after frame, in three
// configurations that between them run every pass (see lit_configs). The UBO is
// vkpt's cvar defaults (UBO_CVAR_LIST) plus the fields vkpt_prepare_frame fills,
// with pt_sharc 0 (but for one configuration), vkpt's camera matrices and a sun behind
// the camera. Unlike the game today a SunColorBuffer is bound, so the sun
// lights the quads (vkpt's physical sky writes it; not ported yet). Checked
// per configuration: TAA_OUTPUT finite everywhere, the quads (the lower half
// with vkpt's projection) lit and brighter than the misses above them; and no
// validation errors throughout. One configuration runs bloom at intensity 1 -
// the rows just above the quads must pick up their light - and one the radiance cache
// (pt_sharc 1, 2 bounces): its update and resolve between the primary rays and
// the lighting, then the cache read back - keys claimed, entries sampled, every
// resolved value finite. Two run DLSS for real (ngx.c, the DLLs beside the game in
// <root>): Super Resolution under A-SVGF, and Ray Reconstruction with TAA folded
// into the interleave pass - the 64x32 render upscaled into the 128x64 DLSS_OUTPUT
// with a Halton jitter, then bloom's and tone mapping's DLSS arm on that. Checked:
// every creation and evaluation succeeds, DLSS_OUTPUT finite with the quads lit
// over the misses. NGX does not start on an Agility SDK factory's device (device.c),
// so they run only with `system` - the device on the system runtime, as the game makes
// it under pt_dlss; otherwise, or without NGX, they are skipped and say so. After
// Super Resolution, frame generation on its images (ngx_dlssg.cpp, as frontend.c's
// d3d12_frontend_dlssg_record runs it): the feature with one generated frame, then
// with MultiFrameCountMax's when the GPU has more, two frames each - DLSS_OUTPUT
// encoded to sRGB (dlssg_hdr.comp), every generated frame evaluated, all decoded.
// Checked: every creation and evaluation succeeds, each generated frame finite and
// lit, DLSS_OUTPUT back as it was to within the round trip.
//
// Phase 3, the physical sky: sky.c with the real tables (the env/*.dds of
// q2rtx_media.pkz, unpacked into a directory given as env=<dir>) and the
// Stroggos atmosphere (precomputed_sky_params.h):
//   * every DDS through d3d12_dds_parse, its shape and format checked
//   * the terrain shadow map, drawn by d3d12_record_sky from the real
//     heightmap's grid, against a CPU rasterizer with Vulkan's rules
//     (shadow_map.vert's y flip, Vulkan's viewport, front faces culled with
//     clockwise = front): the texels it covers and their depths; the opposite
//     cull is reported too, to show the comparison can tell them apart
//   * physical_sky.comp and sky_buffer_resolve.comp: the envmap's six layers
//     finite, the sky above lit over the ground, the sun and sky colours resolved
//   * lit frames under ENVIRONMENT_DYNAMIC with the resolved sun colour bound
//     as world.c binds it (the sky recorded on frames 0 and 3 only, as
//     needs_update would): the sky in the misses, the sun on the quads; then
//     ENVIRONMENT_STATIC with a skybox cube (TEX_ENVMAP)
//
// Phase 4, reflect_refract: a second world with a chrome mirror facing a
// chrome wall behind the camera. After one bounce every mirror pixel's surface
// is exactly where its ray lands on the wall, after two where it lands back on
// the mirror (PT_SHADING_POSITION and its material); the picture finite. Then
// both again over tile lists (TILE COMPACTION, in rows of 3 groups so that these
// few tiles take several rows and a part-empty last one): the surfaces must be
// the full grid's to the bit, and each list the mirror's tiles, rows and all.
//
// Phase 5, dynamic models, as world.c records a frame with monsters in it:
//   * animate_materials.comp (d3d12_record_instance_geometry) over a world of
//     two-frame materials: each primitive below num_static_primitives steps to
//     its next frame, the one past it stays
//   * a two-frame vertex-animated model laid out as vertex_buffer.c lays one
//     out (primitive_buffers[VERTEX_BUFFER_FIRST_MODEL]) and a ModelInstance
//     drawing it between the frames, turned and moved: instance_geometry.comp
//     writes the instanced primitives and positions where the CPU puts them
//     (the pose lerp, the transform, the motion to transform_prev, the
//     instance's material)
//   * accel.c's per-frame BLASes over the instanced positions (an empty section
//     and one at an offset, as vkpt_pt_create_all_dynamic's), planned before
//     the TLAS's instance list names them, then a TLAS of phase 1's world and
//     the model as vkpt_pt_append_dynamic_instances adds it; the lit frame's
//     primary rays hit the model where its ray meets it, with its material
// Phase 6, the effects TLAS: a particle, a sprite, a beam and an explosion in
//   world.c's transparency buffers, their BLASes as d3d12_world_plan_dynamic
//   lays them out, the second TLAS after the geometry's; PT_TRANSPARENT holds
//   each effect's colour where it is and nothing anywhere else
// Phase 7, blood: d3d12_record_blood_upload (world.c's upload_blood) over an
//   epoch frame, an ordinary one and an empty one, every byte of both
//   instanced buffers checked against the same frames applied on the CPU
// Phase 8, the fog (god_rays.c, shadow_map.c) over phase 1's world:
//   * passes.c's d3d12_record_shadow_map into a two-layer sun shadow map: the
//     quads, one moved by its draw's matrix, and one as the water layer; the
//     depth where each projects is the CPU's, and 1 where nothing does
//   * the lit frame with no fog, then the classic god rays (the march, the
//     filter): PT_TRANSPARENT finite, and lit over the misses where it was not
//   * map fog (fog_mode 2) on the froxel grid: ReSTIR's two passes, a sky
//     exposure bake batch, scatter, integrate, both marches, the filter; the
//     integrated volume finite and not empty, the baked voxels written
//
// Build and run from a vcvars64 prompt, from the repository root (NGX: the DLSS
// SDK the game builds with, DLSS_SDK2_PATH; frame generation's half is C++):
//   cl /nologo /O2 /EHsc /D_CRT_SECURE_NO_WARNINGS /I extern/agility-sdk/include /I %DLSS_SDK2_PATH%/include src/refresh/d3d12/tests/d3d12_world_test.c src/refresh/d3d12/ngx_dlssg.cpp %DLSS_SDK2_PATH%/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib /Fe:build/tools/ /Fo:build/tools/
//   build/tools/d3d12_world_test.exe C:/Q2RTX/ [nogbv] [system] [env=<dir>]   (exit 0 = every pixel right, validation quiet)
// Without env= phase 3 is skipped (and says so). Headless: no window.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bindings.c"
#include "../accel.c"
#include "../nvapi_rt.c"
#include "../../vkpt/ptlas.c"
#include "../../vkpt/clusters.c"
#include "../passes.c"
#include "../sky.c"
#include "../ngx.c"
#include <nvsdk_ngx_defs_dlssg.h>
#include <float.h>
#include "../../vkpt/precomputed_sky_params.h"

#define W               64
#define H               32
#define HEAP_BINDLESS   MAX_RIMAGES
#define HEAP_SETS       16384
#define SAMPLER_HEAP    2048
#define TAN_HALF_FOV    1.0f

static int errors;
static ID3D12Device5 *dev;
// `system`: the system runtime's GPU-based validation flags 1358 on the path
// tracer's first read of ASVGF_RNG_SEED (a simultaneous-access texture in COMMON,
// which D3D12 allows; the Agility layer passes the same stream) - counted here
static bool count_system_1358;
static int system_1358;

static void __stdcall msg(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id, LPCSTR d, void *ctx)
{
    (void)c; (void)ctx;
    if (id == 989)
        return;
    if (id == 1358 && count_system_1358) {
        system_1358++;
        return;
    }
    if (s <= D3D12_MESSAGE_SEVERITY_WARNING) {
        if (s <= D3D12_MESSAGE_SEVERITY_ERROR)
            errors++;
        if (errors < 40)
            printf("  [%s %d] %s\n", s <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning", (int)id, d);
    }
}

static void *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*size); fread(p, 1, *size, f); fclose(f);
    return p;
}

static ID3D12Resource *buffer(D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                     &IID_ID3D12Resource, (void **)&r))) {
        printf("FAIL: buffer of %llu bytes\n", (unsigned long long)size);
        exit(1);
    }
    return r;
}

static ID3D12Resource *upload(const void *data, uint64_t size)
{
    ID3D12Resource *r = buffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p = NULL;
    ID3D12Resource_Map(r, 0, NULL, &p);
    memset(p, 0, size);
    if (data)
        memcpy(p, data, size);
    ID3D12Resource_Unmap(r, 0, NULL);
    return r;
}

static void transition(ID3D12GraphicsCommandList4 *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x = { 0 };
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &x);
}

static ID3D12CommandQueue *q;
static ID3D12Fence *fence;
static uint64_t fence_value;

static bool run(ID3D12GraphicsCommandList4 *cl)
{
    ID3D12GraphicsCommandList4_Close(cl);
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
    ID3D12CommandQueue_Signal(q, fence, ++fence_value);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, ev);
    bool ok = WaitForSingleObject(ev, 20000) == WAIT_OBJECT_0;
    CloseHandle(ev);
    if (!ok)
        printf("FAIL: the GPU did not finish in 20 s\n");
    return ok && SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev));
}

// a quad at depth z over x in [x0, x1], y in [y0, y1], both windings: 4 prims
static int add_quad(VboPrimitive *p, float x0, float x1, float y0, float y1, float z, uint32_t material)
{
    const float c[4][3] = { { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z } };
    static const int tri[4][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 0, 2, 1 }, { 0, 3, 2 } };
    for (int t = 0; t < 4; t++) {
        memset(&p[t], 0, sizeof(p[t]));
        memcpy(p[t].pos0, c[tri[t][0]], sizeof(vec3));
        memcpy(p[t].pos1, c[tri[t][1]], sizeof(vec3));
        memcpy(p[t].pos2, c[tri[t][2]], sizeof(vec3));
        p[t].material_id = material;
        p[t].cluster = -1;
        p[t].instance = ~0u;
        p[t].emissive_and_alpha = 0x3c003c00u;     // emissive 1, alpha 1: as bsp_mesh.c's opaque faces
    }
    return 4;
}

/* ------------------------------------------------------------------------- */
/* phase 2 helpers                                                            */
/* ------------------------------------------------------------------------- */

#define LIT_TAN_X   1.0f        // square pixels at W x H
#define LIT_TAN_Y   0.5f
#define LIT_ZNEAR   1.0f
#define LIT_ZFAR    10000.0f

static float half_to_float(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0)
        v = ldexpf((float)m, -24);
    else if (e == 31)
        v = m ? NAN : INFINITY;
    else
        v = ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

static void identity(mat4 m)
{
    memset(m, 0, sizeof(mat4));
    for (int i = 0; i < 4; i++)
        m[i][i] = 1.f;
}

// matrix.c's create_projection_matrix and its inverse; GLSL's column-major
// mat4, [column][row]. View space looks down +z; P[1][1] is negative.
static void projection(mat4 P, mat4 invP)
{
    const float c = (LIT_ZFAR + LIT_ZNEAR) / (LIT_ZFAR - LIT_ZNEAR);
    const float d = 2.f * LIT_ZFAR * LIT_ZNEAR / (LIT_ZFAR - LIT_ZNEAR);
    memset(P, 0, sizeof(mat4));
    P[0][0] = 1.f / LIT_TAN_X;
    P[1][1] = -1.f / LIT_TAN_Y;
    P[2][2] = c;
    P[3][2] = d;
    P[2][3] = 1.f;
    memset(invP, 0, sizeof(mat4));
    invP[0][0] = LIT_TAN_X;
    invP[1][1] = -LIT_TAN_Y;
    invP[3][2] = 1.f;
    invP[2][3] = 1.f / d;
    invP[3][3] = -c / d;
}

// one frame's UBO: vkpt's cvar defaults, then what vkpt_prepare_frame fills
static void lit_ubo(QVKUniformBuffer_t *u, int frame, bool denoiser, float bounces, bool restir_gi,
                    uint32_t taa_w, uint32_t taa_h)
{
    memset(u, 0, sizeof(*u));
#ifdef UBO_CVAR_DO
#undef UBO_CVAR_DO
#endif
#define UBO_CVAR_DO(name, default_value) u->name = (float)(default_value);
    UBO_CVAR_LIST
#undef UBO_CVAR_DO

    // the switches, as main.c's prepare_ubo and world.c's copy leave them
    u->flt_enable = denoiser;
    u->flt_taa = AA_MODE_TAA;
    u->pt_dlss = 0;
    u->pt_dlssdn = 0;
    u->pt_num_bounce_rays = bounces;
    u->pt_restir_gi = restir_gi ? 1.f : 0.f;
    u->pt_sharc = 0.f;
    u->temporal_blend_factor = 0.f;
    // prepare_ubo: depth of field (pt_dof 1) only while accumulating, in photo
    // mode; otherwise the aperture is zero. Left at the cvar's 2, every ray off
    // the 200-unit focal plane was jittered across a lens.
    u->pt_aperture = 0.f;

    u->current_frame_idx = frame;
    u->time = frame / 60.f;
    u->time_prev = (frame - 1) / 60.f;
    u->width = W;
    u->height = H;
    u->current_gpu_slice_width = W;
    u->prev_gpu_slice_width = W;
    u->prev_width = W;
    u->prev_height = H;
    u->inv_width = 1.f / W;
    u->inv_height = 1.f / H;
    u->screen_image_width = W;
    u->screen_image_height = H;
    u->unscaled_width = W;
    u->unscaled_height = H;
    u->taa_image_width = (int)taa_w;
    u->taa_image_height = (int)taa_h;
    u->taa_output_width = W;
    u->taa_output_height = H;
    u->prev_taa_output_width = W;
    u->prev_taa_output_height = H;
    u->pt_fullres_fields = 0;
    u->pt_field_offset = W / 2;
    u->pt_prev_field_offset = W / 2;
    u->medium = MEDIUM_NONE;
    u->environment_type = ENVIRONMENT_NONE;
    u->pt_env_scale = 1.f;

    // a still camera at the origin
    identity(u->V);
    identity(u->invV);
    identity(u->V_prev);
    identity(u->environment_rotation_matrix);
    projection(u->P, u->invP);
    projection(u->P_prev, u->invP_prev);
    u->projection_fov_scale[0] = u->projection_fov_scale_prev[0] = LIT_TAN_X;
    u->projection_fov_scale[1] = u->projection_fov_scale_prev[1] = LIT_TAN_Y;

    // the sun: up and behind the camera, so it lights the quads' near side
    const float len = sqrtf(0.6f * 0.6f + 1.f);
    const float dir[3] = { 0.f, 0.6f / len, -1.f / len };
    const float tangent[3] = { 1.f, 0.f, 0.f };
    const float bitangent[3] = {    // cross(dir, tangent)
        dir[1] * tangent[2] - dir[2] * tangent[1],
        dir[2] * tangent[0] - dir[0] * tangent[2],
        dir[0] * tangent[1] - dir[1] * tangent[0],
    };
    const float half_angle = 0.5f * 0.53f * 3.14159265f / 180.f;
    memcpy(u->sun_direction, dir, sizeof(vec3));
    memcpy(u->sun_direction_envmap, dir, sizeof(vec3));
    memcpy(u->sun_tangent, tangent, sizeof(vec3));
    memcpy(u->sun_bitangent, bitangent, sizeof(vec3));
    u->sun_tan_half_angle = tanf(half_angle);
    u->sun_cos_half_angle = cosf(half_angle);
    u->sun_solid_angle = 2.f * 3.14159265f * (1.f - cosf(half_angle));
    u->sun_color[0] = u->sun_color[1] = u->sun_color[2] = 4.f;
    u->sun_bounce_scale = 1.f;
    u->sun_visible = 1;
}

// an rgba16f / rgba32f image's subresource (an array layer), top-left w x h, as floats (rgba)
static bool read_subresource(ID3D12GraphicsCommandList4 *cl, ID3D12CommandAllocator *ca, ID3D12Resource *image,
                             UINT subresource, int w, int h, float *out)
{
    D3D12_RESOURCE_DESC d;
    ID3D12Resource_GetDesc(image, &d);
    if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format != DXGI_FORMAT_R32G32B32A32_FLOAT &&
        d.Format != DXGI_FORMAT_R32_UINT) {
        printf("  (format %d: not read)\n", (int)d.Format);
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total;
    ID3D12Device5_GetCopyableFootprints(dev, &d, subresource, 1, 0, &fp, NULL, NULL, &total);
    fp.Offset = 0;
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
    D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
    to.pResource = rb; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    from.pResource = image; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = subresource;
    ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &to, 0, 0, 0, &from, NULL);
    if (!run(cl)) {
        ID3D12Resource_Release(rb);
        return false;
    }
    uint8_t *map = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&map);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                const uint8_t *row = map + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
                float v;
                if (d.Format == DXGI_FORMAT_R32_UINT) {     // the uint's bits in .x, zeros after
                    v = 0.f;
                    if (c == 0)
                        memcpy(&v, row + (size_t)x * 4, 4);
                } else if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    uint16_t hv;
                    memcpy(&hv, row + ((size_t)x * 4 + c) * 2, 2);
                    v = half_to_float(hv);
                } else {
                    memcpy(&v, row + ((size_t)x * 4 + c) * 4, 4);
                }
                out[((size_t)y * w + x) * 4 + c] = v;
            }
    ID3D12Resource_Unmap(rb, 0, NULL);
    ID3D12Resource_Release(rb);
    return true;
}

// where instance_geometry.comp puts a vertex-animated model's vertex:
// mix(curr pose, prev pose, backlerp), then the instance's transform (GLSL's
// column-major mat4, [column][row])
static void model_vertex(const float curr[3], const float prev[3], float lerp, const float m[16], float out[3])
{
    float p[3];
    for (int i = 0; i < 3; i++)
        p[i] = curr[i] + (prev[i] - curr[i]) * lerp;
    for (int r = 0; r < 3; r++)
        out[r] = m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2] + m[3 * 4 + r];
}

// a buffer's bytes, through a readback buffer (the GPU idle after)
static bool read_buffer(ID3D12GraphicsCommandList4 *cl, ID3D12CommandAllocator *ca, ID3D12Resource *src,
                        uint64_t offset, uint64_t size, void *out)
{
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!rb)
        return false;
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
    ID3D12GraphicsCommandList4_CopyBufferRegion(cl, rb, 0, src, offset, size);
    const bool ok = run(cl);
    if (ok) {
        void *p = NULL;
        ID3D12Resource_Map(rb, 0, NULL, &p);
        memcpy(out, p, size);
        ID3D12Resource_Unmap(rb, 0, NULL);
    } else {
        printf("FAIL: buffer readback\n");
    }
    ID3D12Resource_Release(rb);
    return ok;
}

static bool read_image(ID3D12GraphicsCommandList4 *cl, ID3D12CommandAllocator *ca, ID3D12Resource *image,
                       int w, int h, float *out)
{
    return read_subresource(cl, ca, image, 0, w, h, out);
}

/* ------------------------------------------------------------------------- */
/* phase 8 helpers: the fog                                                   */
/* ------------------------------------------------------------------------- */

// shadow_map.c's vkpt_shadow_map_setup without the jitter: an orthographic view
// down the sun over the world box, square in x and y; column-major. depth_scale:
// the box's depth along the sun.
static void fog_shadow_viewproj(const float sun[3], const float bmin[3], const float bmax[3], float VP[16],
                                float *depth_scale)
{
    float up[3] = { 0.f, 0.f, 1.f };
    if (sun[2] >= 0.99f) {
        up[0] = 1.f;
        up[2] = 0.f;
    }
    float look[3] = { -sun[0], -sun[1], -sun[2] }, left[3];
    float n = sqrtf(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
    for (int i = 0; i < 3; i++)
        look[i] /= n;
    left[0] = up[1] * look[2] - up[2] * look[1];
    left[1] = up[2] * look[0] - up[0] * look[2];
    left[2] = up[0] * look[1] - up[1] * look[0];
    n = sqrtf(left[0] * left[0] + left[1] * left[1] + left[2] * left[2]);
    for (int i = 0; i < 3; i++)
        left[i] /= n;
    up[0] = look[1] * left[2] - look[2] * left[1];
    up[1] = look[2] * left[0] - look[0] * left[2];
    up[2] = look[0] * left[1] - look[1] * left[0];

    const float *rows[3] = { left, up, look };
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < 8; i++) {
        const float c[3] = { (i & 1) ? bmax[0] : bmin[0], (i & 2) ? bmax[1] : bmin[1], (i & 4) ? bmax[2] : bmin[2] };
        for (int r = 0; r < 3; r++) {
            const float v = rows[r][0] * c[0] + rows[r][1] * c[1] + rows[r][2] * c[2];
            lo[r] = fminf(lo[r], v);
            hi[r] = fmaxf(hi[r], v);
        }
    }
    const float m = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
    for (int r = 0; r < 2; r++) {
        const float d = (m - (hi[r] - lo[r])) * 0.5f;
        lo[r] -= d;
        hi[r] += d;
    }
    // matrix.c's create_orthographic_matrix(lo, hi) times the view
    memset(VP, 0, 16 * sizeof(float));
    for (int r = 0; r < 3; r++) {
        const float w = hi[r] - lo[r];
        for (int c = 0; c < 3; c++)
            VP[c * 4 + r] = (r < 2 ? 2.f : 1.f) / w * rows[r][c];
        VP[12 + r] = r < 2 ? -(hi[r] + lo[r]) / w : -lo[r] / w;
    }
    VP[15] = 1.f;
    *depth_scale = hi[2] - lo[2];
}

// where shadow_map.vert puts a point and god_rays.comp's GetShadow looks it up:
// the texel (u = x/2 + 1/2, v = 1/2 - y/2) and the depth (orthographic: w = 1)
static void fog_shadow_texel(const float m[16], const float p[3], int size, int *tx, int *ty, float *depth)
{
    float o[3];
    for (int r = 0; r < 3; r++)
        o[r] = m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2] + m[3 * 4 + r];
    *tx = (int)floorf((o[0] * 0.5f + 0.5f) * size);
    *ty = (int)floorf((0.5f - 0.5f * o[1]) * size);
    *depth = o[2];
}

// one layer of a D32 image (R32_TYPELESS), as floats; the image is in
// NON_PIXEL_SHADER_RESOURCE before and after
static bool read_depth_layer(ID3D12GraphicsCommandList4 *cl, ID3D12CommandAllocator *ca, ID3D12Resource *image,
                             UINT layer, int size, float *out)
{
    D3D12_RESOURCE_DESC d;
    ID3D12Resource_GetDesc(image, &d);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total;
    ID3D12Device5_GetCopyableFootprints(dev, &d, layer, 1, 0, &fp, NULL, NULL, &total);
    fp.Offset = 0;
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!rb)
        return false;
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
    transition(cl, image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
    to.pResource = rb; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    from.pResource = image; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = layer;
    ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &to, 0, 0, 0, &from, NULL);
    transition(cl, image, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool ok = run(cl);
    if (ok) {
        uint8_t *map = NULL;
        ID3D12Resource_Map(rb, 0, NULL, (void **)&map);
        for (int y = 0; y < size; y++)
            memcpy(out + (size_t)y * size, map + (size_t)y * fp.Footprint.RowPitch, (size_t)size * 4);
        ID3D12Resource_Unmap(rb, 0, NULL);
    }
    ID3D12Resource_Release(rb);
    return ok;
}

// a 3D image (simultaneous-access, in COMMON), every slice: how many texels hold
// a non-finite channel, how many a non-zero one, and the largest |channel|.
// R32_UINT texels count as their integer value.
static bool volume_stats(ID3D12GraphicsCommandList4 *cl, ID3D12CommandAllocator *ca, ID3D12Resource *vol,
                         int *nonfinite, int *nonzero, double *peak)
{
    D3D12_RESOURCE_DESC d;
    ID3D12Resource_GetDesc(vol, &d);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size, total;
    ID3D12Device5_GetCopyableFootprints(dev, &d, 0, 1, 0, &fp, &rows, &row_size, &total);
    fp.Offset = 0;
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!rb)
        return false;
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
    D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
    to.pResource = rb; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    from.pResource = vol; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &to, 0, 0, 0, &from, NULL);
    const bool ok = run(cl);
    *nonfinite = *nonzero = 0;
    *peak = 0;
    if (ok) {
        const int comps = d.Format == DXGI_FORMAT_R32_UINT ? 1 : 4;
        const int bytes = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2 : 4;
        uint8_t *map = NULL;
        ID3D12Resource_Map(rb, 0, NULL, (void **)&map);
        for (UINT z = 0; z < fp.Footprint.Depth; z++)
            for (UINT y = 0; y < fp.Footprint.Height; y++)
                for (UINT x = 0; x < fp.Footprint.Width; x++) {
                    const uint8_t *t = map + ((size_t)z * rows + y) * fp.Footprint.RowPitch + (size_t)x * comps * bytes;
                    bool bad = false, any = false;
                    for (int c = 0; c < comps; c++) {
                        float v;
                        if (d.Format == DXGI_FORMAT_R32_UINT) {
                            uint32_t u;
                            memcpy(&u, t, 4);
                            v = (float)u;
                        } else if (bytes == 2) {
                            uint16_t h;
                            memcpy(&h, t + c * 2, 2);
                            v = half_to_float(h);
                        } else {
                            memcpy(&v, t + c * 4, 4);
                        }
                        if (!isfinite(v))
                            bad = true;
                        else if (v != 0.f) {
                            any = true;
                            *peak = fmax(*peak, fabs(v));
                        }
                    }
                    *nonfinite += bad;
                    *nonzero += any;
                }
        ID3D12Resource_Unmap(rb, 0, NULL);
    }
    ID3D12Resource_Release(rb);
    return ok;
}

// phase 8's world box (vkpt's world_aabb): the quads, the camera, air above them
static const float fog_box_min[3] = { -1000.f, -1000.f, 0.f }, fog_box_max[3] = { 1000.f, 1000.f, 400.f };
// the sky exposure grid the test bakes: 8x8x4 voxels of 64 units from the box's corner
#define FOG_EXPOSURE_X  8
#define FOG_EXPOSURE_Y  8
#define FOG_EXPOSURE_Z  4

// what vkpt_god_rays_prepare_ubo and prepare_ubo's map fog put on top of the lit
// frame's UBO: mode 0 none, 1 the classic god rays (no map fog), 2 map fog with
// local lights (cl_fog 3, the froxel grid), the sky exposure bake `ready` or not.
// The sun moves to `sun` (the shadow map's): ahead of the camera, so the view
// rays scatter it forward - from behind, at gr_eccentricity 0.75, the phase
// function leaves under 1% of it.
static void fog_ubo(QVKUniformBuffer_t *u, int mode, const float sun[3], const float VP[16], float depth_scale,
                    bool exposure_ready)
{
    const float tangent[3] = { 1.f, 0.f, 0.f };
    const float bitangent[3] = {    // cross(sun, tangent)
        sun[1] * tangent[2] - sun[2] * tangent[1],
        sun[2] * tangent[0] - sun[0] * tangent[2],
        sun[0] * tangent[1] - sun[1] * tangent[0],
    };
    memcpy(u->sun_direction, sun, sizeof(vec3));
    memcpy(u->sun_direction_envmap, sun, sizeof(vec3));
    memcpy(u->sun_tangent, tangent, sizeof(vec3));
    memcpy(u->sun_bitangent, bitangent, sizeof(vec3));
    for (int i = 0; i < 3; i++) {
        u->world_center[i] = 0.5f * (fog_box_min[i] + fog_box_max[i]);
        u->world_size[i] = fog_box_max[i] - fog_box_min[i];
        u->world_half_size_inv[i] = 2.f / (fog_box_max[i] - fog_box_min[i]);
    }
    u->shadow_map_depth_scale = depth_scale;
    memcpy(u->shadow_map_VP, VP, 16 * sizeof(float));
    u->god_rays_intensity = mode ? 2.f : 0.f;   // gr_intensity
    u->god_rays_eccentricity = 0.75f;           // gr_eccentricity
    u->fog_camera_cluster = -1;
    u->fog_vol_density_ratio = 1.f;
    u->fog_frame_time = 1.f / 60.f;
    u->fog_sky_trace = 1;
    u->pt_reflect_refract = 1.f;
    // pt_fog_sky_exposure's 2 resolved as vkpt_god_rays_prepare_ubo does: the
    // shaders see 0 or 1
    u->pt_fog_sky_exposure = mode == 2 ? 1.f : 0.f;
    for (int i = 0; i < 3; i++)
        u->sky_exposure_origin[i] = fog_box_min[i];
    u->sky_exposure_voxel = 64.f;
    u->sky_exposure_dim_x = FOG_EXPOSURE_X;
    u->sky_exposure_dim_y = FOG_EXPOSURE_Y;
    u->sky_exposure_dim_z = FOG_EXPOSURE_Z;
    u->sky_exposure_ready = exposure_ready ? 1 : 0;
    if (mode == 2) {
        u->fog_enable = 1;
        u->fog_mode = 2;
        // getDensity's fog_density / FOG_DISTANCEFOG_REFERENCE: 1, the classic god rays' world box
        u->fog_density = 2.f;
        u->fog_color_r = u->fog_color_g = u->fog_color_b = 0.5f;
        u->fog_hf_start_r = u->fog_hf_start_g = u->fog_hf_start_b = 0.5f;
        u->fog_hf_end_r = u->fog_hf_end_g = u->fog_hf_end_b = 0.5f;
    }
}

/* ------------------------------------------------------------------------- */
/* phase 3 helpers: the physical sky                                          */
/* ------------------------------------------------------------------------- */

// a parsed DDS as a texture, as world.c's load_sky_dds: every subresource
// copied in from *staging (the caller releases it once the list has run)
static ID3D12Resource *dds_texture(ID3D12GraphicsCommandList4 *cl, const d3d12_dds_t *dds, ID3D12Resource **staging)
{
    D3D12_RESOURCE_DESC rd;
    d3d12_dds_resource_desc(dds, &rd);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     NULL, &IID_ID3D12Resource, (void **)&r))) {
        printf("FAIL: a %ux%ux%u texture (%u layers) of format %d\n", dds->width, dds->height, dds->depth,
               dds->layers, (int)dds->format);
        exit(1);
    }
    const UINT n = dds->depth > 1 ? 1 : dds->layers;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[6];
    UINT rows[6];
    UINT64 total = 0;
    ID3D12Device5_GetCopyableFootprints(dev, &rd, 0, n, 0, fp, rows, NULL, &total);
    *staging = buffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t *map = NULL;
    ID3D12Resource_Map(*staging, 0, NULL, (void **)&map);
    const size_t row = (size_t)dds->width * dds->bytes_per_pixel, each = dds->size / n;
    for (UINT i = 0; i < n; i++)
        for (UINT z = 0; z < fp[i].Footprint.Depth; z++)
            for (UINT y = 0; y < rows[i]; y++)
                memcpy(map + fp[i].Offset + ((size_t)z * rows[i] + y) * fp[i].Footprint.RowPitch,
                       dds->pixels + each * i + ((size_t)z * dds->height + y) * row, row);
    ID3D12Resource_Unmap(*staging, 0, NULL);
    for (UINT i = 0; i < n; i++) {
        D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
        to.pResource = r; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = i;
        from.pResource = *staging; from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = fp[i];
        ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &to, 0, 0, 0, &from, NULL);
    }
    transition(cl, r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return r;
}

// precomputed_sky.c's SkyBuildTerrainGrid, over a parsed heightmap
static uint32_t terrain_grid(const d3d12_dds_t *heightmap, unsigned side, float size_km, float **vertices,
                             uint32_t **indices)
{
    const float *heights = (const float *)heightmap->pixels;
    float *v = malloc((size_t)side * side * 3 * sizeof(float));
    uint32_t *ix = malloc((size_t)(side - 1) * (side - 1) * 6 * sizeof(uint32_t));
    const float delta = size_km / (float)side;
    float X = -0.5f * size_km, Y = -0.5f * size_km;
    for (unsigned y = 0; y < side; y++) {
        for (unsigned x = 0; x < side; x++) {
            const unsigned i = y * side + x;
            v[i * 3 + 0] = X;
            v[i * 3 + 1] = Y;
            v[i * 3 + 2] = (x == 0 || y == 0 || x == side - 1 || y == side - 1) ? -6.f : heights[i] * 3.f;
            X += delta;
        }
        X = -0.5f * size_km;
        Y += delta;
    }
    uint32_t n = 0;
    for (unsigned y = 0; y < side - 1; y++)
        for (unsigned x = 0; x < side - 1; x++) {
            const uint32_t upper = (y + 1) * side + x, lower = y * side + x;
            ix[n++] = lower; ix[n++] = upper + 1; ix[n++] = lower + 1;
            ix[n++] = lower; ix[n++] = upper; ix[n++] = upper + 1;
        }
    *vertices = v;
    *indices = ix;
    return n;
}

// UpdateTerrainShadowMapView: create_centered_orthographic_matrix(+-22.5 km) x
// create_look_at_matrix(origin, -sun, up z); column-major, [column * 4 + row]
static void sky_viewproj(const float sun[3], float m[16])
{
    const float len = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    const float f[3] = { -sun[0] / len, -sun[1] / len, -sun[2] / len };
    float s[3] = { -f[1], f[0], 0.f };     // cross((0, 0, 1), f)
    const float sl = sqrtf(s[0] * s[0] + s[1] * s[1]);
    s[0] /= sl; s[1] /= sl;
    const float u[3] = { f[1] * s[2] - f[2] * s[1], f[2] * s[0] - f[0] * s[2], f[0] * s[1] - f[1] * s[0] };
    float V[16] = { 0 }, P[16] = { 0 };
    V[0] = s[0]; V[4] = s[1]; V[8] = s[2];
    V[1] = u[0]; V[5] = u[1]; V[9] = u[2];
    V[2] = f[0]; V[6] = f[1]; V[10] = f[2];
    V[15] = 1.f;
    const float b = 30.f * 0.75f;
    P[0] = 2.f / (2.f * b);
    P[5] = 2.f / (2.f * b);
    P[10] = 1.f / (2.f * b);
    P[14] = -(1.f / (2.f * b)) * -b;
    P[15] = 1.f;
    for (int i = 0; i < 4; i++)     // matrix.c's mult_matrix_matrix: P x V
        for (int j = 0; j < 4; j++)
            m[i * 4 + j] = P[0 * 4 + j] * V[i * 4 + 0] + P[1 * 4 + j] * V[i * 4 + 1] + P[2 * 4 + j] * V[i * 4 + 2] +
                           P[3 * 4 + j] * V[i * 4 + 3];
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* The terrain shadow map as Vulkan draws it: shadow_map.vert (viewproj, then
   y negated), Vulkan's viewport (framebuffer y = (y_ndc + 1) / 2 * size), the
   triangle's area a = -1/2 sum(x_i y_i+1 - x_i+1 y_i) in framebuffer
   coordinates, front = clockwise = a < 0, the front faces culled (or, with
   cull_front false, the back ones), depth LESS from a 1.0 clear. Texel
   centres on an edge count as inside (where the GPU's top-left rule picks
   one of two triangles sharing it, both have the same depth there). */
static void reference_shadowmap(const float *vertices, const uint32_t *indices, uint32_t num_indices,
                                const float m[16], bool cull_front, float *depth)
{
    const int S = D3D12_SKY_SHADOWMAP_SIZE;
    for (size_t i = 0; i < (size_t)S * S; i++)
        depth[i] = 1.f;
    for (uint32_t t = 0; t + 2 < num_indices; t += 3) {
        float x[3], y[3], z[3];
        for (int k = 0; k < 3; k++) {
            const float *p = vertices + (size_t)indices[t + k] * 3;
            const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
            const float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
            const float cz = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
            x[k] = (cx + 1.f) * 0.5f * S;
            y[k] = (-cy + 1.f) * 0.5f * S;
            z[k] = cz;
        }
        const float a = -0.5f * ((x[0] * y[1] - x[1] * y[0]) + (x[1] * y[2] - x[2] * y[1]) + (x[2] * y[0] - x[0] * y[2]));
        if (a == 0.f || (a < 0.f) == cull_front)
            continue;
        const int x0 = max(0, (int)floorf(fminf(x[0], fminf(x[1], x[2])))), x1 = min(S - 1, (int)ceilf(fmaxf(x[0], fmaxf(x[1], x[2]))));
        const int y0 = max(0, (int)floorf(fminf(y[0], fminf(y[1], y[2])))), y1 = min(S - 1, (int)ceilf(fmaxf(y[0], fmaxf(y[1], y[2]))));
        for (int py = y0; py <= y1; py++)
            for (int px = x0; px <= x1; px++) {
                const float qx = px + 0.5f, qy = py + 0.5f;
                const float w0 = edge(x[1], y[1], x[2], y[2], qx, qy);
                const float w1 = edge(x[2], y[2], x[0], y[0], qx, qy);
                const float w2 = edge(x[0], y[0], x[1], y[1], qx, qy);
                const float sum = w0 + w1 + w2;
                if (sum == 0.f)
                    continue;
                if (sum > 0 ? (w0 < 0 || w1 < 0 || w2 < 0) : (w0 > 0 || w1 > 0 || w2 > 0))
                    continue;
                const float d = (w0 * z[0] + w1 * z[1] + w2 * z[2]) / sum;
                if (d >= 0.f && d <= 1.f && d < depth[(size_t)py * S + px])
                    depth[(size_t)py * S + px] = d;
            }
    }
}

// the sky's bindings of a textures set, as world.c's write_sky_textures_set
static void write_sky_bindings(const d3d12_set_t *set, ID3D12Resource *const tables[7],
                               const D3D12_SHADER_RESOURCE_VIEW_DESC tables_srv[7], ID3D12Resource *envmap,
                               ID3D12Resource *skybox, ID3D12Resource *shadowmap)
{
    static const unsigned bindings[7] = {
        BINDING_OFFSET_SKY_TRANSMITTANCE, BINDING_OFFSET_SKY_SCATTERING, BINDING_OFFSET_SKY_IRRADIANCE,
        BINDING_OFFSET_SKY_CLOUDS, BINDING_OFFSET_TERRAIN_ALBEDO, BINDING_OFFSET_TERRAIN_NORMALS,
        BINDING_OFFSET_TERRAIN_DEPTH,
    };
    D3D12_SAMPLER_DESC sd;
    d3d12_global_sampler_desc(D3D12_GLOBAL_SAMPLER_DEFAULT, &sd);
    for (int i = 0; i < 7; i++)
        d3d12_set_write_srv_image(set, bindings[i], 0, tables[i], &tables_srv[i], &sd);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    d3d12_sky_envmap_srv_desc(&sv);
    d3d12_set_write_srv_image(set, BINDING_OFFSET_PHYSICAL_SKY, 0, envmap, &sv, &sd);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv;
    d3d12_sky_envmap_uav_desc(&uv);
    d3d12_set_write_uav_image(set, BINDING_OFFSET_PHYSICAL_SKY_IMG, 0, envmap, &uv);
    d3d12_sky_skybox_srv_desc(&sv);
    d3d12_set_write_srv_image(set, BINDING_OFFSET_ENVMAP, 0, skybox, &sv, &sd);
    d3d12_sky_shadowmap_srv_desc(&sv);
    d3d12_set_write_srv_image(set, BINDING_OFFSET_TERRAIN_SHADOWMAP, 0, shadowmap, &sv, &sd);
}

// shader/sky.h's (GLSL beside them: not includable here)
#define PHYSICAL_SKY_FLAG_DRAW_MOUNTAINS  (1 << 1)
#define PHYSICAL_SKY_FLAG_DRAW_CLOUDS     (1 << 2)

// vkpt_physical_sky_update_ubo's fields with the cvar defaults and the
// Stroggos preset: the sun 45 degrees up at azimuth 345, 1 degree across.
// The world's sun (sun_direction, which lights the quads) stays lit_ubo's.
static void sky_ubo(QVKUniformBuffer_t *u, int environment)
{
    const float el = 45.f * 3.14159265f / 180.f, az = 345.f * 3.14159265f / 180.f;
    const float dir[3] = { cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el) };
    const float color[3] = { 0.315f * 10.f, 0.137f * 10.f, 0.033f * 10.f };     // sunColor x sun_brightness
    const float ground[3] = { 0.133f, 0.101f, 0.047f };
    const float half_angle = 0.5f * 3.14159265f / 180.f;
    u->environment_type = environment;
    u->physical_sky_flags = PHYSICAL_SKY_FLAG_DRAW_MOUNTAINS | PHYSICAL_SKY_FLAG_DRAW_CLOUDS;
    memcpy(u->sun_direction_envmap, dir, sizeof(vec3));
    memcpy(u->sun_color, color, sizeof(vec3));
    for (int i = 0; i < 3; i++)
        u->physical_sky_ground_radiance[i] = ground[i] * fmaxf(0.f, dir[2]) * color[i];
    u->sun_tan_half_angle = tanf(half_angle);
    u->sun_cos_half_angle = cosf(half_angle);
    u->sun_solid_angle = 2.f * 3.14159265f * (1.f - cosf(half_angle));
    u->sky_transmittance = 10.f;
    u->sky_phase_g = 0.9f;
    u->sky_amb_phase_g = 0.3f;
    u->sky_scattering = 5.f;
    u->pt_env_scale = exp2f(0.f - 2.f);     // physical_sky_brightness 0
    u->planet_albedo_map = 0;
    u->planet_normal_map = 0;
}

static const struct {
    const char  *name;
    bool        denoiser;
    float       bounces;
    bool        caustics, restir_gi;
    int         frames;
    bool        sharc;      // pt_sharc 1: the radiance cache trained, resolved and read
    bool        bloom;      // bloom.c's passes at bloom_intensity 1 (the blurred copy replaces the frame)
    int         dlss;       // 1 Super Resolution, 2 Ray Reconstruction: 2x into DLSS_OUTPUT
} lit_configs[] = {
    // every lighting pass but the second bounce, all of A-SVGF, TAA, tone mapping
    { "A-SVGF, 1 bounce, caustics, ReSTIR GI temporal + spatial", true, 1.f, true, true, 8 },
    // the second-bounce pipeline, plain compositing
    { "compositing, 2 bounces", false, 2.f, false, false, 4 },
    // A-SVGF without its low-frequency passes (they need a bounce)
    { "A-SVGF, no bounce", true, 0.f, false, false, 3 },
    // bloom: the quads' light reaches the misses just above them
    { "A-SVGF, 1 bounce, bloom (bloom_intensity 1)", true, 1.f, false, false, 4, false, true },
    // the radiance cache, at the one bounce count vkpt runs it at (sharc_active);
    // its first frame clears it
    { "A-SVGF, 2 bounces, SHaRC", true, 2.f, false, false, 6, true },
    // DLSS: Super Resolution over A-SVGF (TAA off, as evaluate_taa_settings has it)
    { "DLSS Super Resolution 2x, A-SVGF, 1 bounce", true, 1.f, false, false, 6, false, false, 1 },
    // Ray Reconstruction instead of A-SVGF; the interleave pass writes TAA_OUTPUT
    // (vkpt_taa_folded: no TAA, no A-SVGF, pt_dlss_exact_color)
    { "DLSS Ray Reconstruction 2x, TAA folded, 1 bounce", false, 1.f, false, false, 6, false, false, 2 },
    // the half-height bounce (last: phase 3 takes its sky-less baseline from it)
    { "A-SVGF, half-resolution bounce", true, 0.5f, false, false, 4 },
};

/* Phase 2's DLSS configurations: NGX on the test's device (ngx.c), and passes.c's
   upscale callback as frontend.c's d3d12_frontend_dlss_record makes it - the feature
   the configuration asks for made on its first frame (DLSSDescribeFeature's
   parameters for Performance, which is 2x), then evaluated. */
typedef struct {
    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;
    int                 made, want;         // 1 Super Resolution, 2 Ray Reconstruction
    ID3D12Resource      **images;
    uint32_t            in_w, in_h, out_w, out_h;
    float               jitter_x, jitter_y; // this frame's, as NGX takes it
    bool                reset;
    int                 evaluations, failures;
} test_dlss_t;

static void test_upscale(void *ctx, ID3D12GraphicsCommandList4 *cmd)
{
    test_dlss_t *t = ctx;
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cmd;
    if (t->feature && t->made != t->want) {     // the GPU is idle between the test's frames
        d3d12_ngx_release(t->feature);
        t->feature = NULL;
    }
    if (!t->feature) {
        const int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        NVSDK_NGX_DLSS_Create_Params sr = { 0 };
        sr.Feature.InWidth = t->in_w;
        sr.Feature.InHeight = t->in_h;
        sr.Feature.InTargetWidth = t->out_w;
        sr.Feature.InTargetHeight = t->out_h;
        sr.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf;
        sr.InFeatureCreateFlags = flags | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        NVSDK_NGX_DLSSD_Create_Params rr = { 0 };
        rr.InWidth = t->in_w;
        rr.InHeight = t->in_h;
        rr.InTargetWidth = t->out_w;
        rr.InTargetHeight = t->out_h;
        rr.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf;
        rr.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        rr.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
        rr.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
        rr.InFeatureCreateFlags = flags;
        NVSDK_NGX_Parameter_SetUI(t->params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                  NVSDK_NGX_DLSS_Hint_Render_Preset_M);
        NVSDK_NGX_Parameter_SetUI(t->params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                                  NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F);
        NVSDK_NGX_Parameter_SetUI(t->params, NVSDK_NGX_Parameter_Denoise, 1);
        NVSDK_NGX_Parameter_SetUI(t->params, NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
        NVSDK_NGX_Parameter_SetUI(t->params, NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Unpacked);
        NVSDK_NGX_Parameter_SetUI(t->params, NVSDK_NGX_Parameter_Use_HW_Depth, NVSDK_NGX_DLSS_Depth_Type_Linear);
        const NVSDK_NGX_Result r = d3d12_ngx_create(gl, t->params, t->want == 2, &sr, &rr, &t->feature);
        if (NVSDK_NGX_FAILED(r)) {
            printf("  NGX: %s create failed: 0x%08x\n", t->want == 2 ? "Ray Reconstruction" : "Super Resolution",
                   (unsigned)r);
            t->failures++;
            return;
        }
        t->made = t->want;
    }
    d3d12_ngx_eval_t e = {
        .rr = t->made == 2,
        .color = t->images[VKPT_IMG_TAA_OUTPUT],
        .output = t->images[VKPT_IMG_DLSS_OUTPUT],
        .depth = t->images[VKPT_IMG_DLSS_DEPTH],
        .motion = t->images[VKPT_IMG_PT_DLSS_MOTION],
        .diffuse_albedo = t->images[VKPT_IMG_DLSS_ALBEDO],
        .specular_albedo = t->images[VKPT_IMG_DLSS_SPECULAR_ALBEDO],
        .normals = t->images[VKPT_IMG_DLSS_NORMAL],
        .roughness = t->images[VKPT_IMG_DLSS_ROUGHNESS],
        .specular_hit_distance = t->images[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR],
        .reflection_motion = t->images[VKPT_IMG_DLSS_REFLECT_MOTION],
        .before_transparency = t->images[VKPT_IMG_DLSS_BEFORE_TRANSPARENT],
        .width = t->in_w,
        .height = t->in_h,
        .jitter_x = t->jitter_x,
        .jitter_y = t->jitter_y,
        .reset = t->reset,
    };
    const NVSDK_NGX_Result r = d3d12_ngx_evaluate(gl, t->feature, t->params, &e);
    t->evaluations++;
    if (NVSDK_NGX_FAILED(r)) {
        if (!t->failures)
            printf("  NGX: evaluate failed: 0x%08x\n", (unsigned)r);
        t->failures++;
    }
}

// the radical inverse in `base`: taa_samples' Halton points
static float halton(int i, int base)
{
    float f = 1.f, r = 0.f;
    for (; i > 0; i /= base) {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}

/* What DLSSGDescribeEvaluation tells DLSS-G, for a camera that has not moved: the
   view at the origin looking down +Z, a 90 degree vertical field of view over the 2:1
   render, the infinite reversed projection whose depth is near / z (the FG depth's
   1/z, depthInverted), the motion vectors in UV scaled to render pixels. */
static void test_dlssg_desc(DLSSGEvalDesc *d, uint32_t w, uint32_t h, unsigned int generated, bool reset)
{
    memset(d, 0, sizeof(*d));
    d->renderWidth = w;
    d->renderHeight = h;
    d->mvecScaleX = (float)w;
    d->mvecScaleY = (float)h;
    d->cameraUp[1] = 1.f;
    d->cameraRight[0] = 1.f;
    d->cameraFwd[2] = 1.f;
    d->cameraNear = 1.f;
    d->cameraFar = 4096.f;
    d->cameraFOV = 1.5707963f;
    d->cameraAspectRatio = (float)w / (float)h;
    const float aspect = d->cameraAspectRatio;
    // view to clip: x / aspect, y, z_clip = near, w = z; clip to view, its inverse
    const float p[16] = { 1.f / aspect, 0, 0, 0,  0, 1.f, 0, 0,  0, 0, 0, 1.f,  0, 0, 1.f, 0 };
    const float pinv[16] = { aspect, 0, 0, 0,  0, 1.f, 0, 0,  0, 0, 0, 1.f,  0, 0, 1.f, 0 };
    memcpy(d->cameraViewToClip, p, sizeof(p));
    memcpy(d->clipToCameraView, pinv, sizeof(pinv));
    for (int i = 0; i < 4; i++)
        d->clipToPrevClip[i * 5] = d->prevClipToClip[i * 5] = 1.f;
    d->colorBuffersHDR = 0;
    d->depthInverted = 1;
    d->cameraMotionIncluded = 1;
    d->reset = reset;
    d->linearizedDepthScale = 1.f;      // pt_dlss_fg_depth_scale, _partition, _separation
    d->linearizedDepthNearFarPartition = 600.f;
    d->minRelativeLinearDepthObjectSeparation = 40.f;
    d->motionVectorsInvalidValue = FLT_MIN;
    d->multiFrameCount = generated;
    d->multiFrameIndex = 1;
}

int main(int argc, char **argv)
{
    // unbuffered: a run redirected to a file and stopped part way keeps what it printed
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = argc > 1 ? argv[1] : "C:/Q2RTX/";
    bool gbv = true;
    bool system_runtime = false;    // device.c under pt_dlss: no Agility SDK factory
    bool legacy_barriers = false;   // the passes' barriers: UAV barriers even where enhanced ones exist
    const char *env_dir = NULL;     // phase 3's DDS tables
    for (int i = 2; i < argc; i++) {
        if (!_stricmp(argv[i], "legacy"))
            legacy_barriers = true;
        else if (!_stricmp(argv[i], "system"))
            system_runtime = true;
        else if (!_stricmp(argv[i], "nogbv"))
            gbv = false;
        else if (!_strnicmp(argv[i], "env=", 4))
            env_dir = argv[i] + 4;
    }
    char path[MAX_PATH];

    HMODULE d3d12 = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExA("dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_GET_INTERFACE get = (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE ser =
        (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature");
    HRESULT (WINAPI *mkdxgi)(UINT, REFIID, void **) = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");

    // the runtime as device.c picks it: the Agility SDK's through a device factory,
    // or (`system`, the game under pt_dlss) the system's
    ID3D12DeviceFactory *f = NULL;
    ID3D12Debug1 *dbg = NULL;
    if (system_runtime) {
        PFN_D3D12_GET_DEBUG_INTERFACE getdbg =
            (PFN_D3D12_GET_DEBUG_INTERFACE)(void *)GetProcAddress(d3d12, "D3D12GetDebugInterface");
        getdbg(&IID_ID3D12Debug1, (void **)&dbg);
    } else {
        ID3D12SDKConfiguration1 *cfg = NULL;
        get(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&cfg);
        snprintf(path, sizeof(path), "%sD3D12\\", root);
        if (FAILED(ID3D12SDKConfiguration1_CreateDeviceFactory(cfg, D3D12_SDK_VERSION, path, &IID_ID3D12DeviceFactory, (void **)&f))) {
            printf("no Agility runtime at %s\n", path);
            return 1;
        }
        ID3D12DeviceFactory_GetConfigurationInterface(f, &CLSID_D3D12Debug, &IID_ID3D12Debug1, (void **)&dbg);
    }
    if (dbg) {
        ID3D12Debug1_EnableDebugLayer(dbg);
        ID3D12Debug1_SetEnableGPUBasedValidation(dbg, gbv);
    }
    IDXGIFactory6 *xf = NULL; mkdxgi(0, &IID_IDXGIFactory6, (void **)&xf);
    IDXGIAdapter4 *ad = NULL;
    IDXGIFactory6_EnumAdapterByGpuPreference(xf, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&ad);
    HRESULT dhr;
    if (f)
        dhr = ID3D12DeviceFactory_CreateDevice(f, (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev);
    else
        dhr = ((PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(d3d12, "D3D12CreateDevice"))(
            (IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev);
    if (FAILED(dhr)) {
        printf("CreateDevice failed: 0x%08lx\n", (unsigned long)dhr);
        return 1;
    }
    printf("%s runtime\n", f ? "Agility SDK" : "system");
    count_system_1358 = !f;
    ID3D12InfoQueue1 *iq = NULL;
    if (SUCCEEDED(ID3D12Device5_QueryInterface(dev, &IID_ID3D12InfoQueue1, (void **)&iq))) {
        DWORD cookie; ID3D12InfoQueue1_RegisterMessageCallback(iq, msg, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie);
    }
    printf("GPU-based validation %s\n", gbv ? "on" : "off");

    // the heaps, as device.c lays them out
    ID3D12DescriptorHeap *rheap = NULL, *sheap = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HEAP_BINDLESS + HEAP_SETS,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12Device5_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rheap);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; hd.NumDescriptors = SAMPLER_HEAP;
    ID3D12Device5_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&sheap);
    d3d12_heaps.device = (ID3D12Device *)dev;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rheap, &d3d12_heaps.resource_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(rheap, &d3d12_heaps.resource_gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(sheap, &d3d12_heaps.sampler_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(sheap, &d3d12_heaps.sampler_gpu);
    d3d12_heaps.resource_increment = ID3D12Device5_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d3d12_heaps.sampler_increment = ID3D12Device5_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    d3d12_heaps.resource_next = HEAP_BINDLESS;
    d3d12_heaps.resource_end = HEAP_BINDLESS + HEAP_SETS;
    d3d12_heaps.sampler_next = D3D12_NUM_GLOBAL_SAMPLERS;
    d3d12_heaps.sampler_end = SAMPLER_HEAP;
    for (int i = 0; i < HEAP_BINDLESS; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = d3d12_heaps.resource_cpu;
        h.ptr += (SIZE_T)i * d3d12_heaps.resource_increment;
        ID3D12Device5_CreateShaderResourceView(dev, NULL, &sv, h);
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = d3d12_heaps.sampler_cpu;
        for (int i = 0; i < D3D12_NUM_GLOBAL_SAMPLERS; i++) {
            D3D12_SAMPLER_DESC sd;
            d3d12_global_sampler_desc(i, &sd);
            ID3D12Device5_CreateSampler(dev, &sd, h);
            h.ptr += d3d12_heaps.sampler_increment;
        }
    }

    char err[1024] = "";
    if (FAILED(d3d12_bindings_create_root_signatures((ID3D12Device *)dev, ser, err, sizeof(err)))) {
        printf("ROOT SIGNATURES FAILED: %s\n", err);
        return 1;
    }

    // the G-buffer, as world.c's ensure_images; the output twice the render, which
    // only the DLSS_* images are sized by (phase 2's DLSS configurations)
    d3d12_extents_t ext = { W, H, 2 * W, 2 * H, W, H };
    static ID3D12Resource *img[NUM_VKPT_IMAGES];
    for (int i = 0; i < NUM_VKPT_IMAGES; i++) {
        uint32_t iw, ih;
        d3d12_image_extent(i, &ext, &iw, &ih);
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { 0 };
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = iw; rd.Height = ih; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = d3d12_image_format(i); rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                         NULL, &IID_ID3D12Resource, (void **)&img[i]))) {
            printf("FAIL: image %s\n", d3d12_image_name(i));
            return 1;
        }
    }

    // the world: two double-wound quads over the lower half of the view
    VboPrimitive prims[8];
    int num_prims = 0;
    num_prims += add_quad(prims + num_prims, -1000.f, 0.f, -1000.f, 0.f, 100.f, MATERIAL_KIND_REGULAR | 1);
    num_prims += add_quad(prims + num_prims, 0.f, 1000.f, -1000.f, 0.f, 200.f, MATERIAL_KIND_REGULAR | 2);
    const uint64_t primitives_size = sizeof(VboPrimitive) * num_prims;
    const uint64_t world_size = primitives_size + (uint64_t)num_prims * 9 * sizeof(float);
    uint8_t *world_data = calloc(1, world_size);
    memcpy(world_data, prims, primitives_size);
    float *positions = (float *)(world_data + primitives_size);
    for (int i = 0; i < num_prims; i++) {
        memcpy(positions + i * 9 + 0, prims[i].pos0, sizeof(vec3));
        memcpy(positions + i * 9 + 3, prims[i].pos1, sizeof(vec3));
        memcpy(positions + i * 9 + 6, prims[i].pos2, sizeof(vec3));
    }
    ID3D12Resource *world = buffer(D3D12_HEAP_TYPE_DEFAULT, world_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *world_up = upload(world_data, world_size);

    // the instance buffer: instance 0 is world geometry (model index -1, prims from 0)
    InstanceBuffer *ib = calloc(1, sizeof(InstanceBuffer));
    ib->tlas_instance_model_indices[0] = -1;
    ib->tlas_instance_prim_offsets[0] = 0;
    ID3D12Resource *instance_buffer = buffer(D3D12_HEAP_TYPE_DEFAULT, sizeof(InstanceBuffer),
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *instance_up = upload(ib, sizeof(InstanceBuffer));

    ID3D12Resource *light_buffer = buffer(D3D12_HEAP_TYPE_DEFAULT, sizeof(LightBuffer), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *readback_buffer = buffer(D3D12_HEAP_TYPE_DEFAULT, sizeof(ReadbackBuffer),
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *sampler_table = buffer(D3D12_HEAP_TYPE_DEFAULT, MAX_RIMAGES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_COMMON);

    // the UBO: a camera at the origin looking down +z, rectilinear, checkerboard fields
    const uint64_t ubo_size = (sizeof(QVKUniformBuffer_t) + 255) & ~255ull;
    QVKUniformBuffer_t *ubo = calloc(1, ubo_size);
    ubo->width = W;
    ubo->height = H;
    ubo->current_gpu_slice_width = W;
    ubo->pt_field_offset = W / 2;
    ubo->pt_fullres_fields = 0;
    // invP * (cx, cy, 1, 1) = (cx tan, cy tan, 1, 0): GLSL's column-major mat4, [column][row]
    ubo->invP[0][0] = TAN_HALF_FOV;
    ubo->invP[1][1] = TAN_HALF_FOV;
    ubo->invP[2][2] = 1.f;
    for (int i = 0; i < 4; i++)
        ubo->invV[i][i] = 1.f;
    ID3D12Resource *ubo_buf = upload(ubo, ubo_size);

    // the BLAS over the positions, the TLAS with one geom_opaque-like instance
    d3d12_as_triangles_t geom = { ID3D12Resource_GetGPUVirtualAddress(world) + primitives_size, (uint32_t)num_prims * 3, 0, 0 };
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS bf = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    d3d12_as_sizes_t bs, ts;
    if (!d3d12_blas_sizes(dev, &geom, 1, bf, &bs) || !d3d12_tlas_sizes(dev, 1, bf, &ts)) {
        printf("FAIL: no AS sizes\n");
        return 1;
    }
    d3d12_as_pool_t pool;
    if (!d3d12_as_pool_create(&pool, (ID3D12Device *)dev, bs.result + ts.result + 2 * D3D12_AS_ALIGNMENT, L"test pool")) {
        printf("FAIL: AS pool\n");
        return 1;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS blas = d3d12_as_pool_alloc(&pool, bs.result);
    const D3D12_GPU_VIRTUAL_ADDRESS tlas = d3d12_as_pool_alloc(&pool, ts.result);
    ID3D12Resource *scratch = buffer(D3D12_HEAP_TYPE_DEFAULT, max(bs.scratch, ts.scratch), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_COMMON);
    D3D12_RAYTRACING_INSTANCE_DESC inst = { 0 };
    inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.f;
    inst.InstanceID = VERTEX_BUFFER_WORLD;
    inst.InstanceMask = AS_FLAG_OPAQUE | AS_FLAG_WORLD_STATIC;
    inst.InstanceContributionToHitGroupIndex = SBTO_OPAQUE;
    inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;    // geom_opaque's VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR
    inst.AccelerationStructure = blas;
    ID3D12Resource *inst_up = upload(&inst, sizeof(inst));

    // the sets, as world.c writes them
    d3d12_set_t textures[2], ubo_set, vb_set, rt_set, tiles_set;
    for (int odd = 0; odd < 2; odd++) {
        if (!d3d12_set_alloc(&textures[odd], D3D12_SET_TEXTURES, "textures"))
            return 1;
        textures[odd].external[0] = 0;
        for (int i = 0; i < NUM_VKPT_IMAGES; i++) {
            const int shown = d3d12_image_at_binding(i, odd);
            const unsigned b = d3d12_image_binding(i);
            D3D12_SAMPLER_DESC sd;
            d3d12_global_sampler_desc(d3d12_image_sampler(shown), &sd);
            d3d12_set_write_uav_image(&textures[odd], BINDING_OFFSET_IMAGES + b, 0, img[shown], NULL);
            d3d12_set_write_srv_image(&textures[odd], BINDING_OFFSET_TEXTURES + b, 0, img[shown], NULL, &sd);
        }
        for (int s = 0; s < D3D12_NUM_GLOBAL_SAMPLERS; s++) {
            D3D12_SAMPLER_DESC sd;
            d3d12_global_sampler_desc(s, &sd);
            d3d12_set_write_sampler(&textures[odd], D3D12_BINDING_GLOBAL_SAMPLERS, s, &sd);
        }
        d3d12_set_write_buffer(&textures[odd], D3D12_BINDING_GLOBAL_SAMPLER_INDEX, 0, sampler_table, 0, MAX_RIMAGES);
    }
    if (!d3d12_set_alloc(&ubo_set, D3D12_SET_UBO, "ubo") || !d3d12_set_alloc(&vb_set, D3D12_SET_VERTEX_BUFFER, "vb") ||
        !d3d12_set_alloc(&rt_set, D3D12_SET_RT, "rt") || !d3d12_set_alloc(&tiles_set, D3D12_SET_PT_TILES, "tiles"))
        return 1;
    d3d12_set_write_cbv(&ubo_set, GLOBAL_UBO_BINDING_IDX, 0, ID3D12Resource_GetGPUVirtualAddress(ubo_buf), (uint32_t)ubo_size);
    d3d12_set_write_buffer(&ubo_set, GLOBAL_INSTANCE_BUFFER_BINDING_IDX, 0, instance_buffer, 0, sizeof(InstanceBuffer));
    d3d12_set_write_buffer(&vb_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_WORLD, world, 0, primitives_size);
    d3d12_set_write_buffer(&vb_set, LIGHT_BUFFER_BINDING_IDX, 0, light_buffer, 0, sizeof(LightBuffer));
    d3d12_set_write_buffer(&vb_set, READBACK_BUFFER_BINDING_IDX, 0, readback_buffer, 0, sizeof(ReadbackBuffer));
    // the radiance cache, as world.c makes and binds it
    const uint64_t sharc_sizes[3] = { sizeof(uint32_t) * SHARC_CAPACITY,
                                      sizeof(uint32_t) * SHARC_ACCUM_UINTS * SHARC_CAPACITY,
                                      sizeof(float) * 4 * SHARC_RESOLVED_VEC4S * SHARC_CAPACITY };
    ID3D12Resource *sharc[3];
    for (int i = 0; i < 3; i++)
        sharc[i] = buffer(D3D12_HEAP_TYPE_DEFAULT, sharc_sizes[i], D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COMMON);
    d3d12_set_write_buffer(&vb_set, SHARC_KEY_BUFFER_BINDING_IDX, 0, sharc[0], 0, sharc_sizes[0]);
    d3d12_set_write_buffer(&vb_set, SHARC_ACCUM_BUFFER_BINDING_IDX, 0, sharc[1], 0, sharc_sizes[1]);
    d3d12_set_write_buffer(&vb_set, SHARC_RESOLVED_BUFFER_BINDING_IDX, 0, sharc[2], 0, sharc_sizes[2]);
    d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, tlas);

    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\primary_rays.rgen.query.dxil", root);
    size_t code_size = 0;
    void *code = load(path, &code_size);
    if (!code) { printf("FAIL: no %s\n", path); return 1; }
    ID3D12PipelineState *pso = d3d12_create_compute_pso(D3D12_PL_PATH_TRACER, code, code_size, NULL);
    free(code);
    if (!pso) { printf("FAIL: primary rays PSO\n"); return 1; }

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12Device5_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca);
    ID3D12GraphicsCommandList4 *cl = NULL;
    ID3D12Device5_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList4, (void **)&cl);
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cl;

    // list 1: d3d12_world_load's uploads and builds, then world.c's frame
    ID3D12GraphicsCommandList4_CopyBufferRegion(cl, world, 0, world_up, 0, world_size);
    ID3D12GraphicsCommandList4_CopyBufferRegion(cl, instance_buffer, 0, instance_up, 0, sizeof(InstanceBuffer));
    transition(cl, world, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, instance_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d3d12_blas_build(cl, &geom, 1, bf, blas, ID3D12Resource_GetGPUVirtualAddress(scratch), 0);
    d3d12_as_barrier(cl, &pool);
    {
        D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
        b.UAV.pResource = scratch;      // the TLAS reuses the BLAS's scratch
        ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &b);
    }
    d3d12_tlas_build(cl, ID3D12Resource_GetGPUVirtualAddress(inst_up), 1, bf, tlas, ID3D12Resource_GetGPUVirtualAddress(scratch));
    d3d12_as_barrier(cl, &pool);
    // the shaders read the world's primitives too: a storage buffer, so a UAV
    transition(cl, world, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap *heaps[2] = { rheap, sheap };
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
    ID3D12GraphicsCommandList4_SetPipelineState(cl, pso);
    d3d12_compute_begin(gl, D3D12_PL_PATH_TRACER);
    const d3d12_set_t *sets[5] = { &rt_set, &ubo_set, &textures[0], &vb_set, &tiles_set };
    d3d12_compute_bind_sets(gl, D3D12_PL_PATH_TRACER, sets);
    int32_t push[6] = { -1, 0, 0, 0, 0, 0 };
    d3d12_compute_push_constants(gl, D3D12_PL_PATH_TRACER, push, sizeof(push));
    const int field_width = W / 2;
    d3d12_compute_dispatch(gl, D3D12_PL_PATH_TRACER, (field_width + 7) / 8, (H + 7) / 8, 2);

    // world.c's barrier_all
    ID3D12GraphicsCommandList7 *cl7 = NULL;
    if (SUCCEEDED(ID3D12GraphicsCommandList4_QueryInterface(cl, &IID_ID3D12GraphicsCommandList7, (void **)&cl7))) {
        D3D12_GLOBAL_BARRIER gb = { D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE };
        D3D12_BARRIER_GROUP g = { D3D12_BARRIER_TYPE_GLOBAL, 1 };
        g.pGlobalBarriers = &gb;
        ID3D12GraphicsCommandList7_Barrier(cl7, 1, &g);
        ID3D12GraphicsCommandList7_Release(cl7);
    } else {
        D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
        ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &b);
    }
    printf("barriers: %s\n", cl7 ? "enhanced (global)" : "legacy (UAV)");

    // world.c's display blit: the display set (TAA_OUTPUT's texture = the image
    // shown, here PT_SHADING_POSITION), film_grain.frag, field 0 stretched
    d3d12_set_t display;
    if (!d3d12_set_alloc(&display, D3D12_SET_TEXTURES, "display"))
        return 1;
    display.external[0] = 0;
    for (int i = 0; i < NUM_VKPT_IMAGES; i++) {
        int shown = d3d12_image_at_binding(i, 0);
        const unsigned b = d3d12_image_binding(i);
        d3d12_set_write_uav_image(&display, BINDING_OFFSET_IMAGES + b, 0, img[shown], NULL);
        if (i == VKPT_IMG_TAA_OUTPUT)
            shown = d3d12_image_at_binding(VKPT_IMG_PT_SHADING_POSITION, 0);
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(d3d12_image_sampler(shown), &sd);
        d3d12_set_write_srv_image(&display, BINDING_OFFSET_TEXTURES + b, 0, img[shown], NULL, &sd);
    }
    for (int s = 0; s < D3D12_NUM_GLOBAL_SAMPLERS; s++) {
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(s, &sd);
        d3d12_set_write_sampler(&display, D3D12_BINDING_GLOBAL_SAMPLERS, s, &sd);
    }
    d3d12_set_write_buffer(&display, D3D12_BINDING_GLOBAL_SAMPLER_INDEX, 0, sampler_table, 0, MAX_RIMAGES);

    ID3D12PipelineState *blit = NULL;
    {
        size_t vs_size = 0, ps_size = 0;
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\final_blit.vert.dxil", root);
        void *vs = load(path, &vs_size);
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\film_grain.frag.dxil", root);
        void *ps = load(path, &ps_size);
        if (!vs || !ps) { printf("FAIL: no blit DXIL\n"); return 1; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = { 0 };
        gd.pRootSignature = d3d12_pipeline_layouts[D3D12_PL_UBO_TEXTURES].root_signature;
        gd.VS.pShaderBytecode = vs; gd.VS.BytecodeLength = vs_size;
        gd.PS.pShaderBytecode = ps; gd.PS.BytecodeLength = ps_size;
        gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gd.SampleMask = UINT_MAX;
        gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gd.RasterizerState.DepthClipEnable = TRUE;
        gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.NumRenderTargets = 1;
        gd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        gd.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device5_CreateGraphicsPipelineState(dev, &gd, &IID_ID3D12PipelineState, (void **)&blit))) {
            printf("FAIL: blit PSO\n");
            return 1;
        }
        free(vs); free(ps);
    }

    enum { OW = 128, OH = 64 };
    ID3D12Resource *target = NULL;
    {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { 0 };
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = OW; rd.Height = OH; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                              &IID_ID3D12Resource, (void **)&target);
    }
    ID3D12DescriptorHeap *rtv_heap = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12Device5_CreateDescriptorHeap(dev, &rhd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv);
    ID3D12Device5_CreateRenderTargetView(dev, target, NULL, rtv);

    // final_blit_pc.glsl's FinalBlitPushConstants, as world.c's blit_push_t
    struct {
        int32_t source, src_width, src_height, out_width, out_height, hdr;
        float hdr_white_nits; uint32_t seed; float grain_intensity, grain_sigma_px; int32_t grain_radius;
        int32_t crt_mode, crt_grid_width, crt_grid_height;
        float crt_warp_x, crt_warp_y, crt_corner_radius_px, crt_hard_pix, crt_scan_hard_dark, crt_scan_hard_bright, crt_mask_strength;
        int32_t crt_mask_pitch; float crt_glow, crt_vignette;
    } pc = { 0 };
    pc.source = 0;
    pc.src_width = field_width;
    pc.src_height = H;
    pc.out_width = OW;
    pc.out_height = OH;
    ID3D12GraphicsCommandList4_SetPipelineState(cl, blit);
    d3d12_graphics_begin(gl, D3D12_PL_UBO_TEXTURES);
    const d3d12_set_t *bsets[2] = { &ubo_set, &display };
    d3d12_graphics_bind_sets(gl, D3D12_PL_UBO_TEXTURES, bsets);
    d3d12_graphics_push_constants(gl, D3D12_PL_UBO_TEXTURES, &pc, sizeof(pc));
    D3D12_VIEWPORT vp = { 0.f, 0.f, (float)OW, (float)OH, 0.f, 1.f };
    D3D12_RECT sc = { 0, 0, OW, OH };
    ID3D12GraphicsCommandList4_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList4_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList4_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList4_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList4_DrawInstanced(cl, 4, 1, 0, 0);

    transition(cl, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_DESC td;
    ID3D12Resource_GetDesc(target, &td);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT tfp;
    UINT trows; UINT64 trow_size, ttotal;
    ID3D12Device5_GetCopyableFootprints(dev, &td, 0, 1, 0, &tfp, &trows, &trow_size, &ttotal);
    ID3D12Resource *trb = buffer(D3D12_HEAP_TYPE_READBACK, ttotal, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
        to.pResource = trb; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = tfp;
        from.pResource = target; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
        ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &to, 0, 0, 0, &from, NULL);
    }

    if (!run(cl)) {
        printf("FAIL: primary rays did not complete (device %s)\n",
               SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
        return 1;
    }
    printf("primary rays: %dx%d, two checkerboard fields of %d, done\n", W, H, field_width);

    // list 2: PT_SHADING_POSITION to a readback buffer
    ID3D12Resource *pos_img = img[VKPT_IMG_PT_SHADING_POSITION];
    D3D12_RESOURCE_DESC pd;
    ID3D12Resource_GetDesc(pos_img, &pd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows; UINT64 row_size, total;
    ID3D12Device5_GetCopyableFootprints(dev, &pd, 0, 1, 0, &fp, &rows, &row_size, &total);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
    D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
    to.pResource = rb; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    from.pResource = pos_img; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &to, 0, 0, 0, &from, NULL);
    if (!run(cl)) {
        printf("FAIL: readback\n");
        return 1;
    }
    if (pd.Format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
        printf("FAIL: PT_SHADING_POSITION is format %d, expected R32G32B32A32_FLOAT\n", (int)pd.Format);
        return 1;
    }
    uint8_t *map = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&map);

    // every launch: the pixel it wrote, the ray it traced, what that ray hits
    int bad = 0, hits = 0, misses = 0;
    char picture[H][W + 1];
    for (int z = 0; z < 2; z++)
        for (int ly = 0; ly < H; ly++)
            for (int lx = 0; lx < field_width; lx++) {
                const int ix = lx + z * (W / 2), iy = ly;
                const int sx = z == 0 ? lx * 2 + (ly & 1) : lx * 2 + 1 - (ly & 1);
                const float cx = ((sx + 0.5f) / W) * 2.f - 1.f;
                const float cy = ((ly + 0.5f) / H) * 2.f - 1.f;
                const float dx = cx * TAN_HALF_FOV, dy = cy * TAN_HALF_FOV;
                float ex = 0, ey = 0, ez = 0;
                uint32_t emat = 0;
                if (dy < 0) {
                    ez = dx < 0 ? 100.f : 200.f;
                    emat = dx < 0 ? 1 : 2;
                    ex = dx * ez;
                    ey = dy * ez;
                }
                const float *px = (const float *)(map + fp.Offset + (size_t)iy * fp.Footprint.RowPitch + (size_t)ix * 16);
                uint32_t mat;
                memcpy(&mat, &px[3], 4);
                const uint32_t gmat = mat & MATERIAL_INDEX_MASK;
                bool ok = gmat == emat;
                if (emat)
                    ok = ok && fabsf(px[0] - ex) < 0.05f && fabsf(px[1] - ey) < 0.05f && fabsf(px[2] - ez) < 0.05f;
                picture[iy][sx] = gmat == 0 ? '.' : gmat == 1 ? 'L' : gmat == 2 ? 'R' : '?';
                if (emat) hits++; else misses++;
                if (!ok && bad++ < 12)
                    printf("  pixel (%d,%d) field %d: got (%.2f %.2f %.2f) material 0x%08x, expected (%.2f %.2f %.2f) material %u\n",
                           sx, ly, z, px[0], px[1], px[2], mat, ex, ey, ez, emat);
            }
    ID3D12Resource_Unmap(rb, 0, NULL);

    printf("\nmaterial per screen pixel (L = z 100 quad, R = z 200 quad, . = miss), row 0 first:\n");
    for (int y = 0; y < H; y++) {
        picture[y][W] = 0;
        printf("  %s\n", picture[y]);
    }
    printf("\n%d pixels: %d expected hits, %d expected misses, %d wrong\n", W * H, hits, misses, bad);

    // the blit: output pixel -> field 0's source pixel (nearest) -> its screen
    // pixel; a hit shows its position clamped to [0,1]: blue 1 (z > 1), red 0
    // left of centre (x < 0) and 1 right of it, green 0 (y < 0)
    int blit_bad = 0, blit_checked = 0;
    uint8_t *tmap = NULL;
    ID3D12Resource_Map(trb, 0, NULL, (void **)&tmap);
    for (int oy = 0; oy < OH; oy++)
        for (int ox = 0; ox < OW; ox++) {
            const int src_x = (int)((ox + 0.5f) * field_width / OW), src_y = (int)((oy + 0.5f) * H / OH);
            const int sx = src_x * 2 + (src_y & 1);
            const float cy = ((src_y + 0.5f) / H) * 2.f - 1.f;
            if (cy >= 0)
                continue;   // a miss: whatever the sky store left
            const uint8_t *p = tmap + tfp.Offset + (size_t)oy * tfp.Footprint.RowPitch + (size_t)ox * 4;  // B G R A
            const int er = sx < W / 2 ? 0 : 255;
            blit_checked++;
            if (p[0] != 255 || p[1] != 0 || p[2] != er) {
                if (blit_bad++ < 8)
                    printf("  blit (%d,%d): BGR %d %d %d, expected 255 0 %d\n", ox, oy, p[0], p[1], p[2], er);
            }
        }
    ID3D12Resource_Unmap(trb, 0, NULL);
    printf("blit: %d of %d hit pixels of the %dx%d output wrong\n", blit_bad, blit_checked, OW, OH);

    printf("%d validation errors\n", errors);
    const int phase1_errors = errors;

    /* --------------------------------------------------------------------- */
    /* PHASE 2: the lit frame                                                 */
    /* --------------------------------------------------------------------- */
    printf("\nphase 2: the lit frame (passes.c)\n");
    const bool enhanced = cl7 != NULL && !legacy_barriers;    // released above; only its presence counts
    printf("passes' barriers: %s\n", enhanced ? "enhanced (global)" : "legacy (UAV)");

    // the materials: no texture, so base_factor is the albedo
    LightBuffer *lb = calloc(1, sizeof(LightBuffer));
    for (uint32_t m = 1; m <= 6; m++) {     // 3: phase 4's back wall; 4: phase 5's model
        uint32_t *t = lb->material_table + m * MATERIAL_UINTS;
        t[2] = 0x3c00u | (0xbc00u << 16);   // bump_scale 1, roughness_override -1 (none)
        t[4] = 1u | (m << 16);              // one animation frame, itself next
        t[5] = 0x3c00u | (0x3800u << 16);   // specular_factor 1, base_factor 0.5
        if (m >= 5)                         // phase 5's animated world: two frames, 5 <-> 6
            t[4] = 2u | ((m == 5 ? 6u : 5u) << 16);
    }
    ID3D12Resource *light_up = upload(lb, sizeof(LightBuffer));

    // world.c's lighting resources
    ID3D12Resource *zeros = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_ZERO_BUFFER_SIZE, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *zeros_up = upload(NULL, D3D12_ZERO_BUFFER_SIZE);
    ID3D12Resource *tonemap = buffer(D3D12_HEAP_TYPE_DEFAULT, sizeof(ToneMappingBuffer),
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    const uint64_t stats_size = 4096;
    ID3D12Resource *stats[NUM_LIGHT_STATS_BUFFERS];
    for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
        stats[i] = buffer(D3D12_HEAP_TYPE_DEFAULT, stats_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COMMON);
    const uint64_t sun_size = (sizeof(SunColorBuffer) + 255) & ~255ull;
    SunColorBuffer *sun = calloc(1, sun_size);
    sun->sun_color[0] = sun->sun_color[1] = sun->sun_color[2] = 4.f;
    sun->sun_luminance = 4.f;
    ID3D12Resource *sun_buf = upload(sun, sun_size);

    // textures.c's blue noise: NUM_BLUE_NOISE_TEX layers of R16, random here
    ID3D12Resource *noise = NULL;
    D3D12_RESOURCE_DESC nd = { 0 };
    nd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    nd.Width = BLUE_NOISE_RES; nd.Height = BLUE_NOISE_RES; nd.DepthOrArraySize = NUM_BLUE_NOISE_TEX; nd.MipLevels = 1;
    nd.Format = DXGI_FORMAT_R16_UNORM; nd.SampleDesc.Count = 1;
    {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &nd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         NULL, &IID_ID3D12Resource, (void **)&noise))) {
            printf("FAIL: blue noise texture\n");
            return 1;
        }
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *nfp = calloc(NUM_BLUE_NOISE_TEX, sizeof(*nfp));
    UINT64 noise_total = 0;
    ID3D12Device5_GetCopyableFootprints(dev, &nd, 0, NUM_BLUE_NOISE_TEX, 0, nfp, NULL, NULL, &noise_total);
    ID3D12Resource *noise_up = buffer(D3D12_HEAP_TYPE_UPLOAD, noise_total, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        uint32_t *p = NULL, x = 0x9e3779b9u;
        ID3D12Resource_Map(noise_up, 0, NULL, (void **)&p);
        for (UINT64 i = 0; i < noise_total / 4; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            p[i] = x;
        }
        ID3D12Resource_Unmap(noise_up, 0, NULL);
    }

    // the sets: what world.c writes on top of phase 1's
    for (int odd = 0; odd < 2; odd++) {
        D3D12_SAMPLER_DESC sd;
        d3d12_global_sampler_desc(D3D12_GLOBAL_SAMPLER_DEFAULT, &sd);
        d3d12_set_write_srv_image(&textures[odd], BINDING_OFFSET_BLUE_NOISE, 0, noise, NULL, &sd);
    }
    for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
        d3d12_set_write_buffer(&vb_set, LIGHT_STATS_BUFFER_BINDING_IDX, i, stats[i], 0, stats_size);
    d3d12_set_write_buffer(&vb_set, TONE_MAPPING_BUFFER_BINDING_IDX, 0, tonemap, 0, sizeof(ToneMappingBuffer));
    d3d12_set_write_cbv(&vb_set, SUN_COLOR_UBO_BINDING_IDX, 0, ID3D12Resource_GetGPUVirtualAddress(sun_buf),
                        (uint32_t)sun_size);

    // every pass's pipeline, as world.c's init
    ID3D12PipelineState *psos[D3D12_NUM_PASSES];
    for (int i = 0; i < D3D12_NUM_PASSES; i++) {
        snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\%s.dxil", root, d3d12_pass_info[i].shader);
        size_t size = 0;
        void *dxil = load(path, &size);
        if (!dxil) { printf("FAIL: no %s\n", path); return 1; }
        psos[i] = d3d12_create_compute_pso(d3d12_pass_info[i].layout, dxil, size, NULL);
        free(dxil);
        if (!psos[i]) { printf("FAIL: no pipeline for %s\n", d3d12_pass_info[i].shader); return 1; }
    }

    // the uploads
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
    ID3D12GraphicsCommandList4_CopyBufferRegion(cl, light_buffer, 0, light_up, 0, sizeof(LightBuffer));
    ID3D12GraphicsCommandList4_CopyBufferRegion(cl, zeros, 0, zeros_up, 0, D3D12_ZERO_BUFFER_SIZE);
    for (UINT i = 0; i < NUM_BLUE_NOISE_TEX; i++) {
        D3D12_TEXTURE_COPY_LOCATION nto = { 0 }, nfrom = { 0 };
        nto.pResource = noise; nto.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; nto.SubresourceIndex = i;
        nfrom.pResource = noise_up; nfrom.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; nfrom.PlacedFootprint = nfp[i];
        ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &nto, 0, 0, 0, &nfrom, NULL);
    }
    transition(cl, noise, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!run(cl)) {
        printf("FAIL: phase 2 uploads\n");
        return 1;
    }

    uint32_t taa_w, taa_h;
    d3d12_image_extent(VKPT_IMG_TAA_OUTPUT, &ext, &taa_w, &taa_h);
    float *pixels = calloc((size_t)W * H * 4, sizeof(float));
    float *dlss_pixels = calloc((size_t)4 * W * H * 4, sizeof(float));     // DLSS_OUTPUT, 2W x 2H
    int frame = 0, lit_failed = 0;

    // NGX for the DLSS configurations, the feature DLLs from <root>
    test_dlss_t tdlss = { .images = img, .in_w = W, .in_h = H, .out_w = 2 * W, .out_h = 2 * H };
    bool ngx_up = false;
    {
        wchar_t dll_path[MAX_PATH];
        // as GetFolderPath gives it: backslashes, no trailing one
        MultiByteToWideChar(CP_UTF8, 0, root, -1, dll_path, MAX_PATH);
        size_t n = wcslen(dll_path);
        for (size_t i = 0; i < n; i++)
            if (dll_path[i] == L'/')
                dll_path[i] = L'\\';
        while (n > 3 && dll_path[n - 1] == L'\\')
            dll_path[--n] = 0;
        CreateDirectoryA("DLSSTemp", NULL);     // NGX's data and logs, beside the run
        const NVSDK_NGX_Result r = d3d12_ngx_init((ID3D12Device *)dev, "FFA5FAF5-2329-44AB-A423-3D9B3B177C88",
                                                  "d3d12_world_test", dll_path, L"DLSSTemp/", NULL, &tdlss.params);
        ngx_up = NVSDK_NGX_SUCCEED(r);
        if (!ngx_up)
            printf("  NGX: init with feature DLLs from %ls failed: 0x%08x\n", dll_path, (unsigned)r);
        int sr = 0, rr = 0;
        if (ngx_up) {
            NVSDK_NGX_Parameter_GetI(tdlss.params, NVSDK_NGX_Parameter_SuperSampling_Available, &sr);
            NVSDK_NGX_Parameter_GetI(tdlss.params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rr);
        }
        printf("  NGX on D3D12: %s (Super Resolution %s, Ray Reconstruction %s)\n",
               ngx_up ? "up" : "NOT up - the DLSS configurations are skipped", sr ? "available" : "not available",
               rr ? "available" : "not available");
    }
    double skyless_misses = 0;      // the last configuration's misses: phase 3's baseline
    double prev_edge = 0;           // the configuration before's rows above the quads (bloom's check)
    for (size_t c = 0; c < sizeof(lit_configs) / sizeof(lit_configs[0]); c++) {
        const int errors_before = errors;
        const int dlss = lit_configs[c].dlss;
        if (dlss && !ngx_up) {
            printf("  %-56s skipped: no NGX%s\n", lit_configs[c].name,
                   system_runtime ? "" : " (not on an Agility SDK factory's device: run with `system`)");
            continue;
        }
        const int dlss_failures_before = tdlss.failures, dlss_evaluations_before = tdlss.evaluations;
        for (int k = 0; k < lit_configs[c].frames; k++, frame++) {
            const int odd = frame & 1;
            // taa_samples' jitter, in [-0.5, 0.5); NGX takes it negated (pt_dlss_jitter_sign -1)
            const float jx = halton(frame % 16 + 1, 2) - 0.5f, jy = halton(frame % 16 + 1, 3) - 0.5f;
            {
                void *p = NULL;
                ID3D12Resource_Map(ubo_buf, 0, NULL, &p);
                QVKUniformBuffer_t *u = p;
                lit_ubo(u, frame, lit_configs[c].denoiser, lit_configs[c].bounces,
                        lit_configs[c].restir_gi, taa_w, taa_h);
                u->pt_sharc = lit_configs[c].sharc ? 1.f : 0.f;
                u->bloom_intensity = lit_configs[c].bloom ? 1.f : 0.f;
                if (dlss) {
                    // prepare_ubo under DLSSEnabled (pt_dlss 1, Performance), and
                    // evaluate_taa_settings' AA_MODE_OFF
                    u->pt_dlss = 1.f;
                    u->pt_dlssdn = dlss == 2 ? 1.f : 0.f;
                    u->flt_taa = AA_MODE_OFF;
                    u->unscaled_width = 2 * W;
                    u->unscaled_height = 2 * H;
                    u->sub_pixel_jitter[0] = jx;
                    u->sub_pixel_jitter[1] = jy;
                }
                ID3D12Resource_Unmap(ubo_buf, 0, NULL);
            }
            tdlss.want = dlss;
            tdlss.jitter_x = -jx;
            tdlss.jitter_y = -jy;
            tdlss.reset = k == 0;
            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);

            // world.c: this frame's light statistics start from zero (all three on a new map)
            for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
                if (frame == 0 || i == frame % NUM_LIGHT_STATS_BUFFERS)
                    d3d12_clear_buffer(cl, stats[i], stats_size, zeros);

            const d3d12_passes_t p = {
                .cmd = cl,
                .enhanced_barriers = enhanced,
                .psos = psos,
                .pt_sets = { &rt_set, &ubo_set, &textures[odd], &vb_set, &tiles_set },
                .standard_sets = { &ubo_set, &textures[odd], &vb_set },
                .tonemap_buffer = tonemap,
                .tonemap_size = sizeof(ToneMappingBuffer),
                .zero_buffer = zeros,
                .sharc_buffers = { sharc[0], sharc[1], sharc[2] },
                .sharc_sizes = { sharc_sizes[0], sharc_sizes[1], sharc_sizes[2] },
                .sharc_entries = SHARC_CAPACITY,
                .upscale = test_upscale,
                .upscale_ctx = &tdlss,
                .heaps = heaps,
                .num_heaps = 2,
            };
            const d3d12_passes_frame_t pf = {
                .dlss = dlss != 0,
                .output_width = 2 * W,
                .output_height = 2 * H,
                .taa_folded = dlss == 2,
                .field_width = W / 2,
                .slice_width = W,
                .render_width = W,
                .render_height = H,
                .taa_output_width = W,
                .taa_output_height = H,
                .taa_image_width = taa_w,
                .taa_image_height = taa_h,
                .enable_denoiser = lit_configs[c].denoiser,
                .num_bounce_rays = lit_configs[c].bounces,
                .caustics = lit_configs[c].caustics,
                .restir_gi = lit_configs[c].restir_gi,
                .restir_gi_spatial = lit_configs[c].restir_gi,     // pt_restir_gi_spatial's default is 4
                .tone_mapping = true,
                // and after the bloom configuration, whose bright frames would
                // otherwise leave their eye adaptation to everything after them
                .tm_reset = frame == 0 || (k == 0 && c > 0 && lit_configs[c - 1].bloom),
                .frame_time = 1.f / 60.f,
                .tm_slope_blur_sigma = 6.f,
                .tm_knee_start = 0.9f,
                .tm_white_point = 10.f,
                .sharc = lit_configs[c].sharc,
                .sharc_clear = lit_configs[c].sharc && k == 0,
                .sharc_stride = 4,      // pt_sharc_stride's default
                // vkpt_bloom_blur_params at H 64: bloom_sigma 0.037 x 64 / 4 = 0.59,
                // clamped to 1 - so -1/2, 1/sqrt(2 pi), 4 samples
                .bloom = lit_configs[c].bloom,
                .bloom_argument_scale = -0.5f,
                .bloom_normalization_scale = 0.39894228f,
                .bloom_num_samples = 4,
            };
            d3d12_record_passes(&p, &pf);
            if (!run(cl)) {
                printf("FAIL: frame %d (%s) did not complete (device %s)\n", frame, lit_configs[c].name,
                       SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
                return 1;
            }
        }

        // the last frame's picture - TAA_OUTPUT, or DLSS_OUTPUT at twice the size:
        // finite, the quads lit, brighter than the misses
        const int last_odd = (frame - 1) & 1;
        const int shown = dlss ? VKPT_IMG_DLSS_OUTPUT : VKPT_IMG_TAA_OUTPUT;
        const int sw = dlss ? 2 * W : W, sh = dlss ? 2 * H : H;
        float *const shown_pixels = dlss ? dlss_pixels : pixels;
        if (!read_image(cl, ca, img[d3d12_image_at_binding(shown, last_odd)], sw, sh, shown_pixels)) {
            printf("FAIL: %s readback\n", d3d12_image_name(shown));
            return 1;
        }
        int nonfinite = 0;
        double lum[2] = { 0, 0 };   // [0] the upper half's rows (misses), [1] the rest (the quads)
        double edge = 0;            // the misses 2..4 rows above the quads: bloom's reach, beyond TAA's
        double row_lum[H];          // each row's mean (TAA_OUTPUT's)
        memset(row_lum, 0, sizeof(row_lum));
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                const float *px = shown_pixels + ((size_t)y * sw + x) * 4;
                if (!isfinite(px[0]) || !isfinite(px[1]) || !isfinite(px[2]) || !isfinite(px[3])) {
                    nonfinite++;
                    continue;
                }
                const double l = 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
                lum[y >= sh / 2] += l;
                if (!dlss && y >= H / 2 - 4 && y <= H / 2 - 2)
                    edge += l;
                if (!dlss)
                    row_lum[y] += l / W;
            }
        edge /= (double)W * 3;
        lum[0] /= (double)sw * (sh / 2);
        lum[1] /= (double)sw * (sh - sh / 2);

        // the HDR picture before TAA and tone mapping (stored x STORAGE_SCALE_HDR),
        // over the quads: get_sunlight's sun 4 x N.L / pi x (1 - F0 0.04), times
        // albedo 0.5, plus a little specular from roughness 1
        const double sun_nl = 1.0 / sqrt(0.6 * 0.6 + 1.0);
        const double expected_hdr = 0.5 * 4.0 * sun_nl / 3.14159265 * (1.0 - 0.04);
        int flat_nonfinite = -1;
        double flat_lum = 0;
        if (read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_FLAT_COLOR, last_odd)], W, H, pixels)) {
            flat_nonfinite = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    const float *px = pixels + ((size_t)y * W + x) * 4;
                    if (!isfinite(px[0]) || !isfinite(px[1]) || !isfinite(px[2]))
                        flat_nonfinite++;
                    else if (y >= H / 2)
                        flat_lum += 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
                }
            flat_lum /= (double)W * (H - H / 2) * STORAGE_SCALE_HDR;
        }

        // bloom: those rows lit, well past what the configuration before left there
        bool bloom_ok = true;
        char bloom_note[96] = "";
        if (lit_configs[c].bloom) {
            // ...and local: the top row, past the blur's 4-sample kernel, stays dark
            bloom_ok = edge > 0.02 && edge > 4.0 * prev_edge && row_lum[0] < 0.5 * edge;
            snprintf(bloom_note, sizeof(bloom_note), " | above the quads %.3f (%.3f before)", edge, prev_edge);
        }
        prev_edge = edge;

        // DLSS: every creation and evaluation succeeded, one evaluation a frame
        bool dlss_ok = true;
        char dlss_note[96] = "";
        if (dlss) {
            const int evals = tdlss.evaluations - dlss_evaluations_before, fails = tdlss.failures - dlss_failures_before;
            dlss_ok = fails == 0 && evals == lit_configs[c].frames;
            snprintf(dlss_note, sizeof(dlss_note), " | NGX %d evaluations, %d failures", evals, fails);
        }

        // the radiance cache after its frames: trained (keys claimed) and resolved
        // (entries with samples), every resolved value finite
        bool sharc_ok = true;
        char sharc_note[128] = "";
        if (lit_configs[c].sharc) {
            uint32_t *keys = malloc(sharc_sizes[0]);
            float *resolved = malloc(sharc_sizes[2]);
            if (!keys || !resolved || !read_buffer(cl, ca, sharc[0], 0, sharc_sizes[0], keys) ||
                !read_buffer(cl, ca, sharc[2], 0, sharc_sizes[2], resolved))
                return 1;
            uint32_t claimed = 0, sampled = 0, bad_values = 0;
            for (uint32_t i = 0; i < SHARC_CAPACITY; i++) {
                claimed += keys[i] != 0;
                const float *e = resolved + (size_t)i * 4 * SHARC_RESOLVED_VEC4S;
                for (int v = 0; v < 4 * SHARC_RESOLVED_VEC4S; v++)
                    bad_values += !isfinite(e[v]) || e[v] < 0.f;
                sampled += e[3] > 0.f;
            }
            free(keys);
            free(resolved);
            sharc_ok = claimed > 0 && sampled > 0 && bad_values == 0;
            snprintf(sharc_note, sizeof(sharc_note), " | cache %u keys, %u sampled, %u bad values", claimed, sampled,
                     bad_values);
        }

        const bool ok = nonfinite == 0 && flat_nonfinite == 0 && lum[1] > 0.02 && lum[1] > lum[0] &&
                        fabs(flat_lum - expected_hdr) < 0.15 * expected_hdr && errors == errors_before && sharc_ok && bloom_ok && dlss_ok;
        printf("  %-56s %d frames: %s %d non-finite, quads %.3f misses %.3f | FLAT_COLOR %d non-finite,"
               " quads %.3f HDR (sun alone %.3f) | %d validation errors%s%s%s%s\n",
               lit_configs[c].name, lit_configs[c].frames, d3d12_image_name(shown), nonfinite, lum[1], lum[0],
               flat_nonfinite, flat_lum, expected_hdr, errors - errors_before, sharc_note, bloom_note, dlss_note,
               ok ? "" : "  <-- FAIL");
        if (!ok)
            lit_failed++;
        skyless_misses = lum[0];

        // frame generation on Super Resolution's last frame (the comment at the top)
        if (dlss == 1) {
            int fg_available = 0;
            unsigned int fg_max = 1;
            NVSDK_NGX_Parameter_GetI(tdlss.params, NVSDK_NGX_Parameter_FrameGeneration_Available, &fg_available);
            if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetUI(tdlss.params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
                                                           &fg_max)) || fg_max < 1)
                fg_max = 1;
            if (fg_max > 5)
                fg_max = 5;
            if (!fg_available)
                printf("  %-56s skipped: not available on this GPU\n", "DLSS-G frame generation");
            static const int fg_out[6] = { 0, VKPT_IMG_DLSS_FG_OUTPUT, VKPT_IMG_DLSS_FG_OUTPUT2,
                                           VKPT_IMG_DLSS_FG_OUTPUT3, VKPT_IMG_DLSS_FG_OUTPUT4,
                                           VKPT_IMG_DLSS_FG_OUTPUT5 };
            const unsigned int counts[2] = { 1, fg_max > 1 ? fg_max : 0 };
            const int ow = 2 * W, oh = 2 * H;
            float *before = malloc((size_t)ow * oh * 4 * sizeof(float));
            memcpy(before, dlss_pixels, (size_t)ow * oh * 4 * sizeof(float));
            for (int n = 0; fg_available && n < 2 && counts[n]; n++) {
                const unsigned int gen = counts[n];
                const int errors_fg = errors;
                int fg_evals = 0, fg_fails = 0;
                NVSDK_NGX_Handle *fg = NULL;
                const d3d12_passes_t fp = {
                    .cmd = cl,
                    .enhanced_barriers = enhanced,
                    .psos = psos,
                    .standard_sets = { &ubo_set, &textures[last_odd], &vb_set },
                    .heaps = heaps,
                    .num_heaps = 2,
                };
                for (int k = 0; k < 2; k++) {
                    ID3D12CommandAllocator_Reset(ca);
                    ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
                    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
                    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cl;
                    if (!fg) {
                        const NVSDK_NGX_Result r = d3d12_ngx_dlssg_create(gl, tdlss.params, ow, oh, W, H,
                                                                          DXGI_FORMAT_R16G16B16A16_FLOAT, gen, &fg);
                        if (NVSDK_NGX_FAILED(r)) {
                            printf("  NGX: frame generation create (%u generated) failed: 0x%08x\n", gen, (unsigned)r);
                            fg = NULL;
                            fg_fails++;
                        }
                    }
                    if (fg) {
                        DLSSGEvalDesc d;
                        test_dlssg_desc(&d, W, H, gen, k == 0);
                        d3d12_barrier_all(cl, enhanced);
                        d3d12_record_dlssg_convert(&fp, 2, 0, ow, oh);
                        d3d12_ngx_dlssg_eval_t e = {
                            .backbuffer = img[VKPT_IMG_DLSS_OUTPUT],
                            .depth = img[VKPT_IMG_DLSS_FG_DEPTH],
                            .motion = img[VKPT_IMG_PT_DLSS_MOTION],
                            .desc = &d,
                            .frame_id = (uint64_t)frame + (uint64_t)k,
                        };
                        e.hudless = e.backbuffer;
                        for (unsigned int g = 1; g <= gen; g++) {
                            d.multiFrameIndex = g;
                            e.output = img[fg_out[g]];
                            const NVSDK_NGX_Result r = d3d12_ngx_dlssg_evaluate(gl, fg, tdlss.params, &e);
                            fg_evals++;
                            if (NVSDK_NGX_FAILED(r)) {
                                if (!fg_fails)
                                    printf("  NGX: frame generation evaluate %u/%u failed: 0x%08x\n", g, gen,
                                           (unsigned)r);
                                fg_fails++;
                            }
                        }
                        // NGX left its heaps on the list
                        ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
                        d3d12_barrier_all(cl, enhanced);
                        d3d12_record_dlssg_convert(&fp, 3, 0, ow, oh);
                        for (unsigned int g = 1; g <= gen; g++)
                            d3d12_record_dlssg_convert(&fp, 3, g, ow, oh);
                    }
                    if (!run(cl)) {
                        printf("FAIL: frame generation (%u generated) did not complete (device %s)\n", gen,
                               SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
                        return 1;
                    }
                }
                if (fg)
                    d3d12_ngx_release(fg);      // the GPU is idle after run()

                // each generated frame: finite, the quads lit over the misses
                int gen_nonfinite = 0;
                double gen_lum[2] = { 0, 0 };
                for (unsigned int g = 1; g <= gen && fg_fails == 0; g++) {
                    if (!read_image(cl, ca, img[fg_out[g]], ow, oh, dlss_pixels)) {
                        printf("FAIL: DLSS_FG_OUTPUT %u readback\n", g);
                        return 1;
                    }
                    for (int y = 0; y < oh; y++)
                        for (int x = 0; x < ow; x++) {
                            const float *px = dlss_pixels + ((size_t)y * ow + x) * 4;
                            if (!isfinite(px[0]) || !isfinite(px[1]) || !isfinite(px[2])) {
                                gen_nonfinite++;
                                continue;
                            }
                            gen_lum[y >= oh / 2] += (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) / gen;
                        }
                }
                gen_lum[0] /= (double)ow * (oh / 2);
                gen_lum[1] /= (double)ow * (oh - oh / 2);

                // DLSS_OUTPUT after two encode / decode round trips: what it was
                double worst = 0;
                if (!read_image(cl, ca, img[VKPT_IMG_DLSS_OUTPUT], ow, oh, dlss_pixels)) {
                    printf("FAIL: DLSS_OUTPUT readback\n");
                    return 1;
                }
                for (size_t i = 0; i < (size_t)ow * oh * 4; i++) {
                    if (i % 4 == 3 || !isfinite(before[i]))
                        continue;
                    // the encode clamps to [0, 1] (dlssg_hdr.comp), as vkpt's does
                    const double want = before[i] < 0.f ? 0.0 : before[i] > 1.f ? 1.0 : (double)before[i];
                    const double diff = fabs((double)dlss_pixels[i] - want) / (want + 0.01);
                    if (!isfinite(dlss_pixels[i]) || diff > worst)
                        worst = isfinite(dlss_pixels[i]) ? diff : 1e9;
                }

                const bool fg_ok = fg_fails == 0 && fg_evals == 2 * (int)gen && gen_nonfinite == 0 &&
                                   gen_lum[1] > 0.02 && gen_lum[1] > gen_lum[0] && worst < 0.02 &&
                                   errors == errors_fg;
                char name[64];
                snprintf(name, sizeof(name), "DLSS-G frame generation %ux (%u generated)", gen + 1, gen);
                printf("  %-56s 2 frames: %d evaluations, %d failures | generated %d non-finite, quads %.3f"
                       " misses %.3f | DLSS_OUTPUT round trip %.2f%% | %d validation errors%s\n",
                       name, fg_evals, fg_fails, gen_nonfinite, gen_lum[1], gen_lum[0], 100.0 * worst,
                       errors - errors_fg, fg_ok ? "" : "  <-- FAIL");
                if (!fg_ok)
                    lit_failed++;
            }
            memcpy(dlss_pixels, before, (size_t)ow * oh * 4 * sizeof(float));
            free(before);
        }
    }

    /* SER (pt_ser; world.c's ser_ready, passes.c's trace_raygen): the indirect lighting
       bounces as ray generation shaders - without the reorder (pt_ser 2) and with
       MaybeReorderThread (pt_ser 1) - against the compute passes, over the same three
       frames as "compositing, 2 bounces". Runs in the order 0, 0, 2, 1: each run after
       the first starts from the history an identical run left, so the second compute
       run is the reference and the first shows only whether that history matters. The
       shader has no wave operations, so which lanes run together cannot change a pixel -
       but a library is other DXIL than the compute shader, and a driver that contracts
       its float math differently can flip a random decision (roulette, light choice) on
       a pixel. So: the mean within 1%, at most 1% of the pixels off by more than 1%,
       and the bit-identical share shown. */
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_9 };
        if (FAILED(ID3D12Device5_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
            sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
        if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_9) {
            printf("  %-56s skipped: shader model %d.%d, not 6.9%s\n", "SER (pt_ser 1, 2)", sm.HighestShaderModel >> 4,
                   sm.HighestShaderModel & 15, system_runtime ? " (the system runtime)" : "");
        } else {
            const int errors_ser = errors;
            // [pt_ser 2's raygen, pt_ser 1's ser][first bounce, second]; records 64 bytes apart
            ID3D12StateObject *so[2][2] = { { NULL, NULL }, { NULL, NULL } };
            ID3D12Resource *table = upload(NULL, 4 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
            uint8_t *table_map = NULL;
            ID3D12Resource_Map(table, 0, NULL, (void **)&table_map);
            bool made = true;
            for (int v = 0; v < 2; v++)
                for (int k = 0; k < 2; k++) {
                    snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\indirect_lighting.rgen.query%s.%s.dxil", root,
                             k ? ".spec_0_1" : "", v ? "ser" : "raygen");
                    size_t size = 0;
                    void *code = load(path, &size);
                    so[v][k] = code ? d3d12_create_raygen_object(D3D12_PL_PATH_TRACER, code, size,
                                                                 table_map + (v * 2 + k) * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
                                                                 NULL)
                                    : NULL;
                    if (!so[v][k]) {
                        printf("  SER: no state object from %s\n", path);
                        made = false;
                    }
                    free(code);
                }
            ID3D12Resource_Unmap(table, 0, NULL);
            const D3D12_GPU_VIRTUAL_ADDRESS table_va = ID3D12Resource_GetGPUVirtualAddress(table);

            static const int modes[4] = { 0, 0, 2, 1 };
            float *runs[4] = { NULL, NULL, NULL, NULL };
            const int first_frame = frame;
            for (int m = 0; m < 4 && made; m++) {
                for (int k = 0; k < 3; k++) {
                    const int fnum = first_frame + k;
                    const int odd = fnum & 1;
                    {
                        void *mp = NULL;
                        ID3D12Resource_Map(ubo_buf, 0, NULL, &mp);
                        QVKUniformBuffer_t *u = mp;
                        lit_ubo(u, fnum, false, 2.f, false, taa_w, taa_h);
                        u->pt_sharc = 0.f;
                        u->bloom_intensity = 0.f;
                        ID3D12Resource_Unmap(ubo_buf, 0, NULL);
                    }
                    ID3D12CommandAllocator_Reset(ca);
                    ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
                    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
                    // every run's light statistics from zero
                    for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
                        if (k == 0 || i == fnum % NUM_LIGHT_STATS_BUFFERS)
                            d3d12_clear_buffer(cl, stats[i], stats_size, zeros);
                    d3d12_passes_t sp = {
                        .cmd = cl,
                        .enhanced_barriers = enhanced,
                        .psos = psos,
                        .pt_sets = { &rt_set, &ubo_set, &textures[odd], &vb_set, &tiles_set },
                        .standard_sets = { &ubo_set, &textures[odd], &vb_set },
                        .tonemap_buffer = tonemap,
                        .tonemap_size = sizeof(ToneMappingBuffer),
                        .zero_buffer = zeros,
                        .heaps = heaps,
                        .num_heaps = 2,
                    };
                    if (modes[m]) {
                        const int v = modes[m] == 1 ? 1 : 0;
                        for (int b = 0; b < 2; b++) {
                            sp.raygen[b] = so[v][b];
                            sp.raygen_records[b] = table_va + (uint64_t)(v * 2 + b) * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
                        }
                    }
                    const d3d12_passes_frame_t spf = {
                        .field_width = W / 2, .slice_width = W, .render_width = W, .render_height = H,
                        .taa_output_width = W, .taa_output_height = H, .taa_image_width = taa_w, .taa_image_height = taa_h,
                        .enable_denoiser = false, .num_bounce_rays = 2.f,
                        .tone_mapping = true, .tm_reset = k == 0, .frame_time = 1.f / 60.f,
                        .tm_slope_blur_sigma = 6.f, .tm_knee_start = 0.9f, .tm_white_point = 10.f,
                    };
                    d3d12_record_passes(&sp, &spf);
                    if (!run(cl)) {
                        printf("FAIL: SER run %d (pt_ser %d) frame %d did not complete (device %s)\n", m, modes[m], k,
                               SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
                        return 1;
                    }
                }
                if (!read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_FLAT_COLOR, (first_frame + 2) & 1)], W, H,
                                pixels))
                    return 1;
                runs[m] = malloc((size_t)W * H * 4 * sizeof(float));
                memcpy(runs[m], pixels, (size_t)W * H * 4 * sizeof(float));
            }
            frame = first_frame + 3;

            if (!made) {
                printf("  %-56s no state objects  <-- FAIL\n", "SER (pt_ser 1, 2)");
                lit_failed++;
            } else {
                // each run against the reference (runs[1], compute after a compute run)
                for (int m = 0; m < 4; m++) {
                    if (m == 1)
                        continue;
                    const float *a = runs[m], *ref = runs[1];
                    size_t same = 0, off = 0, nonfinite = 0;
                    double sum_a = 0, sum_ref = 0;
                    for (size_t px = 0; px < (size_t)W * H; px++) {
                        bool pixel_off = false;
                        for (int ch = 0; ch < 3; ch++) {
                            const float x = a[px * 4 + ch], y = ref[px * 4 + ch];
                            if (!isfinite(x)) {
                                nonfinite++;
                                continue;
                            }
                            same += memcmp(&x, &y, sizeof(float)) == 0;
                            sum_a += x;
                            sum_ref += y;
                            pixel_off = pixel_off || fabsf(x - y) > 0.01f * fabsf(y) + 1e-4f;
                        }
                        off += pixel_off;
                    }
                    const double mean_diff = sum_ref > 0 ? fabs(sum_a - sum_ref) / sum_ref : (sum_a > 0 ? 1.0 : 0.0);
                    const bool ok = nonfinite == 0 && mean_diff < 0.01 && off * 100 <= (size_t)W * H;
                    char name[64];
                    snprintf(name, sizeof(name), m == 0 ? "SER reference: compute, after other history"
                                                        : "SER: pt_ser %d against compute", modes[m]);
                    printf("  %-56s FLAT_COLOR %.2f%% bit-identical, %zu of %d pixels off by > 1%%, mean %+.3f%%, "
                           "%zu non-finite%s\n", name, 100.0 * same / (3.0 * W * H), off, W * H,
                           100.0 * (sum_ref > 0 ? (sum_a - sum_ref) / sum_ref : 0.0), nonfinite,
                           m == 0 ? " (information only)" : ok ? "" : "  <-- FAIL");
                    if (m != 0 && !ok)
                        lit_failed++;
                }
                printf("  %-56s %d validation errors%s\n", "SER (pt_ser 1, 2)", errors - errors_ser,
                       errors == errors_ser ? "" : "  <-- FAIL");
                if (errors != errors_ser)
                    lit_failed++;
            }
            for (int m = 0; m < 4; m++)
                free(runs[m]);
            for (int v = 0; v < 2; v++)
                for (int k = 0; k < 2; k++)
                    if (so[v][k])
                        ID3D12StateObject_Release(so[v][k]);
            ID3D12Resource_Release(table);
        }
    }

    /* --------------------------------------------------------------------- */
    /* PHASE 3: the physical sky                                              */
    /* --------------------------------------------------------------------- */
    int sky_failed = 0;
    if (!env_dir) {
        printf("\nphase 3 SKIPPED: no env=<directory> holding the env/*.dds of q2rtx_media.pkz\n");
    } else {
        printf("\nphase 3: the physical sky (sky.c), Stroggos, tables from %s\n", env_dir);
        const int errors_before = errors;

        // the DDS files: world.c's seven tables in its order, then the heightmap
        static const struct {
            const char  *file;
            DXGI_FORMAT format;
            uint32_t    width, height, depth, layers;
        } want[] = {
            { "transmittance_stroggos.dds", DXGI_FORMAT_R32G32B32A32_FLOAT, 256, 64, 1, 1 },
            { "inscatter_stroggos.dds",     DXGI_FORMAT_R32G32B32A32_FLOAT, 256, 128, 32, 1 },
            { "irradiance_stroggos.dds",    DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 16, 1, 1 },
            { "clouds.dds",                 DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, 128, 1 },     // B8G8R8A8 in the file
            { "terrain_albedo.dds",         DXGI_FORMAT_R8G8B8A8_UNORM, 1024, 1024, 1, 6 },     // legacy header, masks
            { "terrain_normal.dds",         DXGI_FORMAT_R8G8B8A8_UNORM, 1024, 1024, 1, 6 },
            { "terrain_depth.dds",          DXGI_FORMAT_R32_FLOAT, 1024, 1024, 1, 6 },
            { "terrain_heightmap.dds",      DXGI_FORMAT_R32_FLOAT, 1024, 1024, 1, 1 },
        };
        enum { NUM_TABLES = 7, NUM_FILES = 8 };
        void *files[NUM_FILES];
        d3d12_dds_t dds[NUM_FILES];
        for (int i = 0; i < NUM_FILES; i++) {
            snprintf(path, sizeof(path), "%s/%s", env_dir, want[i].file);
            size_t size = 0;
            files[i] = load(path, &size);
            if (!files[i]) { printf("FAIL: no %s\n", path); return 1; }
            char e[160];
            if (!d3d12_dds_parse(files[i], size, &dds[i], e, sizeof(e))) { printf("FAIL: %s: %s\n", want[i].file, e); return 1; }
            const bool shape = dds[i].format == want[i].format && dds[i].width == want[i].width &&
                               dds[i].height == want[i].height && dds[i].depth == want[i].depth &&
                               dds[i].layers == want[i].layers && dds[i].cube == (want[i].layers == 6);
            printf("  %-28s %4ux%-4u depth %3u, %u layer%s%s, format %2d%s\n", want[i].file, dds[i].width, dds[i].height,
                   dds[i].depth, dds[i].layers, dds[i].layers > 1 ? "s" : "", dds[i].cube ? " (cube)" : "",
                   (int)dds[i].format, shape ? "" : "  <-- FAIL");
            if (!shape)
                sky_failed++;
        }

        // the images world.c's sky_init makes
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd;
        ID3D12Resource *envmap = NULL, *shadowmap = NULL, *skybox = NULL;
        d3d12_sky_envmap_desc(&rd);
        if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                         NULL, &IID_ID3D12Resource, (void **)&envmap))) {
            printf("FAIL: the envmap\n");
            return 1;
        }
        D3D12_CLEAR_VALUE clear;
        d3d12_sky_shadowmap_desc(&rd, &clear);
        if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                         &clear, &IID_ID3D12Resource, (void **)&shadowmap))) {
            printf("FAIL: the shadow map\n");
            return 1;
        }
        ID3D12DescriptorHeap *dsv_heap = NULL;
        D3D12_DESCRIPTOR_HEAP_DESC dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
        ID3D12Device5_CreateDescriptorHeap(dev, &dhd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv;
        d3d12_sky_shadowmap_dsv_desc(&dv);
        ID3D12Device5_CreateDepthStencilView(dev, shadowmap, &dv, dsv);
        // R_SetSky's skybox, 4x4 faces of mid grey
        d3d12_sky_skybox_desc(4, 4, &rd);
        if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         NULL, &IID_ID3D12Resource, (void **)&skybox))) {
            printf("FAIL: the skybox\n");
            return 1;
        }
        const uint64_t sky_sun_size = (sizeof(SunColorBuffer) + 255) & ~255ull;
        ID3D12Resource *sun_color = buffer(D3D12_HEAP_TYPE_DEFAULT, sky_sun_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_COMMON);
        uint8_t atmosphere_cb[256] = { 0 };
        memcpy(atmosphere_cb, &Params_Stroggos, sizeof(Params_Stroggos));
        ID3D12Resource *atmosphere = upload(atmosphere_cb, sizeof(atmosphere_cb));

        // the terrain grid
        float *grid_v = NULL;
        uint32_t *grid_i = NULL;
        const unsigned side = 1024;
        const uint32_t grid_n = terrain_grid(&dds[7], side, 30.f, &grid_v, &grid_i);
        const uint64_t grid_vbytes = (uint64_t)side * side * 3 * sizeof(float);
        const uint64_t grid_ioffset = (grid_vbytes + 255) & ~255ull;
        const uint64_t grid_size = grid_ioffset + (uint64_t)grid_n * sizeof(uint32_t);
        ID3D12Resource *grid = buffer(D3D12_HEAP_TYPE_DEFAULT, grid_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *grid_up = buffer(D3D12_HEAP_TYPE_UPLOAD, grid_size, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_GENERIC_READ);
        {
            uint8_t *p = NULL;
            ID3D12Resource_Map(grid_up, 0, NULL, (void **)&p);
            memcpy(p, grid_v, grid_vbytes);
            memcpy(p + grid_ioffset, grid_i, (size_t)grid_n * sizeof(uint32_t));
            ID3D12Resource_Unmap(grid_up, 0, NULL);
        }
        const D3D12_GPU_VIRTUAL_ADDRESS grid_va = ID3D12Resource_GetGPUVirtualAddress(grid);
        const D3D12_VERTEX_BUFFER_VIEW grid_vbv = { grid_va, (UINT)grid_vbytes, 3 * sizeof(float) };
        const D3D12_INDEX_BUFFER_VIEW grid_ibv = { grid_va + grid_ioffset, grid_n * (UINT)sizeof(uint32_t),
                                                   DXGI_FORMAT_R32_UINT };

        // the uploads, world.c's load_sky_dds and sky_init's clear, in one list
        ID3D12CommandAllocator_Reset(ca);
        ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
        ID3D12Resource *tables[NUM_TABLES], *staging[NUM_TABLES];
        D3D12_SHADER_RESOURCE_VIEW_DESC tables_srv[NUM_TABLES];
        for (int i = 0; i < NUM_TABLES; i++) {
            tables[i] = dds_texture(cl, &dds[i], &staging[i]);
            d3d12_dds_srv_desc(&dds[i], &tables_srv[i]);
        }
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, grid, 0, grid_up, 0, grid_size);
        ID3D12GraphicsCommandList4_ClearDepthStencilView(cl, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, NULL);
        transition(cl, shadowmap, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uint8_t grey[4 * 4 * 4 * 6];
        memset(grey, 128, sizeof(grey));
        ID3D12Resource *skybox_up = upload(NULL, 6 * 1024);   // 6 faces, 256-byte rows, 1024 apart
        {
            uint8_t *p = NULL;
            ID3D12Resource_Map(skybox_up, 0, NULL, (void **)&p);
            for (int f = 0; f < 6; f++)
                for (int y = 0; y < 4; y++)
                    memcpy(p + f * 1024 + y * 256, grey + (f * 16 + y * 4) * 4, 16);
            ID3D12Resource_Unmap(skybox_up, 0, NULL);
            for (UINT f = 0; f < 6; f++) {
                D3D12_TEXTURE_COPY_LOCATION sto = { 0 }, sfrom = { 0 };
                sto.pResource = skybox; sto.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sto.SubresourceIndex = f;
                sfrom.pResource = skybox_up; sfrom.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                sfrom.PlacedFootprint.Offset = f * 1024;
                sfrom.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sfrom.PlacedFootprint.Footprint.Width = 4;
                sfrom.PlacedFootprint.Footprint.Height = 4;
                sfrom.PlacedFootprint.Footprint.Depth = 1;
                sfrom.PlacedFootprint.Footprint.RowPitch = 256;
                ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &sto, 0, 0, 0, &sfrom, NULL);
            }
            transition(cl, skybox, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        if (!run(cl)) {
            printf("FAIL: phase 3 uploads\n");
            return 1;
        }
        for (int i = 0; i < NUM_TABLES; i++)
            ID3D12Resource_Release(staging[i]);
        ID3D12Resource_Release(grid_up);
        ID3D12Resource_Release(skybox_up);

        // the sets, as world.c's sky_init and write_sky_sets
        d3d12_set_t sky_set;
        if (!d3d12_set_alloc(&sky_set, D3D12_SET_SKY, "sky"))
            return 1;
        d3d12_set_write_cbv(&sky_set, 0, 0, ID3D12Resource_GetGPUVirtualAddress(atmosphere), 256);
        for (int odd = 0; odd < 2; odd++)
            write_sky_bindings(&textures[odd], tables, tables_srv, envmap, skybox, shadowmap);
        d3d12_set_write_buffer(&vb_set, SUN_COLOR_BUFFER_BINDING_IDX, 0, sun_color, 0, sizeof(SunColorBuffer));
        d3d12_set_write_cbv(&vb_set, SUN_COLOR_UBO_BINDING_IDX, 0, ID3D12Resource_GetGPUVirtualAddress(sun_color),
                            (uint32_t)sky_sun_size);

        // the pipelines
        // (the space sky's is made as world.c makes it, on the physical sky's
        // layout, though nothing here draws it)
        ID3D12PipelineState *sky_pso = NULL, *resolve_pso = NULL, *smap_pso = NULL, *space_pso = NULL;
        {
            static const char *const names[4] = { "physical_sky.comp", "sky_buffer_resolve.comp", "shadow_map.vert",
                                                   "physical_sky_space.comp" };
            void *code3[4];
            size_t size3[4];
            for (int i = 0; i < 4; i++) {
                snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\%s.dxil", root, names[i]);
                code3[i] = load(path, &size3[i]);
                if (!code3[i]) { printf("FAIL: no %s\n", path); return 1; }
            }
            sky_pso = d3d12_create_compute_pso(D3D12_PL_PHYSICAL_SKY, code3[0], size3[0], NULL);
            resolve_pso = d3d12_create_compute_pso(D3D12_PL_UBO_VERTEX_BUFFER, code3[1], size3[1], NULL);
            space_pso = d3d12_create_compute_pso(D3D12_PL_PHYSICAL_SKY, code3[3], size3[3], NULL);
            D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
            D3D12_INPUT_ELEMENT_DESC element;
            d3d12_sky_shadowmap_pso_desc(&gd, &element, d3d12_pipeline_layouts[D3D12_PL_SHADOW_MAP].root_signature,
                                         code3[2], size3[2]);
            if (FAILED(ID3D12Device5_CreateGraphicsPipelineState(dev, &gd, &IID_ID3D12PipelineState, (void **)&smap_pso)))
                smap_pso = NULL;
            for (int i = 0; i < 4; i++)
                free(code3[i]);
            if (!sky_pso || !resolve_pso || !smap_pso || !space_pso) {
                printf("FAIL: the sky's pipelines (sky %p, resolve %p, shadow map %p, space %p)\n", (void *)sky_pso,
                       (void *)resolve_pso, (void *)smap_pso, (void *)space_pso);
                return 1;
            }
        }

        // the shadow map's view: vkpt's, through the sun of sky_ubo
        float viewproj[16];
        {
            QVKUniformBuffer_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            sky_ubo(&tmp, ENVIRONMENT_DYNAMIC);
            sky_viewproj(tmp.sun_direction_envmap, viewproj);
        }

        // the frames: the physical sky (recorded on 0 and 3, as needs_update would
        // ask), then the skybox
        enum { SKY_FRAMES = 6, STATIC_FRAMES = 3 };
        double sky_lum[2] = { 0, 0 }, static_lum[2] = { 0, 0 }, sky_flat = 0, static_flat_misses = 0;
        int sky_nonfinite = 0;
        for (int k = 0; k < SKY_FRAMES + STATIC_FRAMES; k++, frame++) {
            const int odd = frame & 1;
            const bool dynamic = k < SKY_FRAMES;
            {
                void *p = NULL;
                ID3D12Resource_Map(ubo_buf, 0, NULL, &p);
                lit_ubo((QVKUniformBuffer_t *)p, frame, true, 1.f, false, taa_w, taa_h);
                sky_ubo((QVKUniformBuffer_t *)p, dynamic ? ENVIRONMENT_DYNAMIC : ENVIRONMENT_STATIC);
                ID3D12Resource_Unmap(ubo_buf, 0, NULL);
            }
            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
            for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
                if (i == frame % NUM_LIGHT_STATS_BUFFERS)
                    d3d12_clear_buffer(cl, stats[i], stats_size, zeros);

            if (k == 0 || k == 3) {
                d3d12_sky_pass_t s = {
                    .cmd = cl,
                    .enhanced_barriers = enhanced,
                    .shadowmap_pso = smap_pso,
                    .shadowmap = shadowmap,
                    .shadowmap_dsv = dsv,
                    .grid_vertices = grid_vbv,
                    .grid_indices = grid_ibv,
                    .grid_index_count = grid_n,
                    .sky_pso = sky_pso,
                    .resolve_pso = resolve_pso,
                    .sets = { &ubo_set, &textures[odd], &vb_set, &sky_set },
                    .sun_color_buffer = sun_color,
                    .sun_color_size = sky_sun_size,
                    .zero_buffer = zeros,
                };
                memcpy(s.viewproj, viewproj, sizeof(viewproj));
                d3d12_record_sky(&s);
            }

            const d3d12_passes_t p = {
                .cmd = cl,
                .enhanced_barriers = enhanced,
                .psos = psos,
                .pt_sets = { &rt_set, &ubo_set, &textures[odd], &vb_set, &tiles_set },
                .standard_sets = { &ubo_set, &textures[odd], &vb_set },
                .tonemap_buffer = tonemap,
                .tonemap_size = sizeof(ToneMappingBuffer),
                .zero_buffer = zeros,
            };
            const d3d12_passes_frame_t pf = {
                .field_width = W / 2, .slice_width = W, .render_width = W, .render_height = H,
                .taa_output_width = W, .taa_output_height = H, .taa_image_width = taa_w, .taa_image_height = taa_h,
                .enable_denoiser = true, .num_bounce_rays = 1.f,
                .tone_mapping = true, .tm_reset = k == 0, .frame_time = 1.f / 60.f,
                .tm_slope_blur_sigma = 6.f, .tm_knee_start = 0.9f, .tm_white_point = 10.f,
            };
            d3d12_record_passes(&p, &pf);
            if (!run(cl)) {
                printf("FAIL: sky frame %d did not complete (device %s)\n", k,
                       SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
                return 1;
            }

            // the last frame of each: TAA_OUTPUT's misses (the upper half) and quads, FLAT_COLOR's
            if (k == SKY_FRAMES - 1 || k == SKY_FRAMES + STATIC_FRAMES - 1) {
                double *l = dynamic ? sky_lum : static_lum;
                if (!read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_TAA_OUTPUT, odd)], W, H, pixels))
                    return 1;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        const float *px = pixels + ((size_t)y * W + x) * 4;
                        if (!isfinite(px[0]) || !isfinite(px[1]) || !isfinite(px[2]))
                            sky_nonfinite++;
                        else
                            l[y >= H / 2] += (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) / ((double)W * (H / 2));
                    }
                if (!read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_FLAT_COLOR, odd)], W, H, pixels))
                    return 1;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        const float *px = pixels + ((size_t)y * W + x) * 4;
                        if (!isfinite(px[0]) || !isfinite(px[1]) || !isfinite(px[2])) {
                            sky_nonfinite++;
                            continue;
                        }
                        const double v = (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) /
                                         ((double)W * (H / 2) * STORAGE_SCALE_HDR);
                        if (dynamic && y >= H / 2)
                            sky_flat += v;
                        else if (!dynamic && y < H / 2)
                            static_flat_misses += v;
                    }
            }
        }

        // the shadow map against Vulkan's rules
        const int S = D3D12_SKY_SHADOWMAP_SIZE;
        float *gpu_depth = malloc((size_t)S * S * sizeof(float));
        float *ref = malloc((size_t)S * S * sizeof(float));
        {
            D3D12_RESOURCE_DESC sd;
            ID3D12Resource_GetDesc(shadowmap, &sd);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT sfp;
            UINT64 stotal;
            ID3D12Device5_GetCopyableFootprints(dev, &sd, 0, 1, 0, &sfp, NULL, NULL, &stotal);
            ID3D12Resource *srb = buffer(D3D12_HEAP_TYPE_READBACK, stotal, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            transition(cl, shadowmap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION sto = { 0 }, sfrom = { 0 };
            sto.pResource = srb; sto.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; sto.PlacedFootprint = sfp;
            sfrom.pResource = shadowmap; sfrom.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sfrom.SubresourceIndex = 0;
            ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &sto, 0, 0, 0, &sfrom, NULL);
            transition(cl, shadowmap, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (!run(cl))
                return 1;
            uint8_t *m = NULL;
            ID3D12Resource_Map(srb, 0, NULL, (void **)&m);
            for (int y = 0; y < S; y++)
                memcpy(gpu_depth + (size_t)y * S, m + sfp.Offset + (size_t)y * sfp.Footprint.RowPitch, (size_t)S * 4);
            ID3D12Resource_Unmap(srb, 0, NULL);
            ID3D12Resource_Release(srb);
        }
        size_t covered = 0, wrong = 0, wrong_opposite = 0;
        reference_shadowmap(grid_v, grid_i, grid_n, viewproj, true, ref);
        for (size_t i = 0; i < (size_t)S * S; i++) {
            covered += gpu_depth[i] < 1.f;
            wrong += fabsf(gpu_depth[i] - ref[i]) > 1e-3f;
        }
        reference_shadowmap(grid_v, grid_i, grid_n, viewproj, false, ref);
        for (size_t i = 0; i < (size_t)S * S; i++)
            wrong_opposite += fabsf(gpu_depth[i] - ref[i]) > 1e-3f;
        const double texels = (double)S * S;
        const bool smap_ok = covered > texels / 100 && wrong < texels / 500 && wrong_opposite > 10 * (wrong + 1);
        printf("  terrain shadow map: %u triangles, %.1f%% of %dx%d texels drawn; %zu differ from Vulkan's rules "
               "(%.3f%%), %zu from the opposite cull%s\n", grid_n / 3, 100.0 * covered / texels, S, S, wrong,
               100.0 * wrong / texels, wrong_opposite, smap_ok ? "" : "  <-- FAIL");
        if (!smap_ok)
            sky_failed++;
        free(gpu_depth);
        free(ref);

        // the resolved sun and sky colours
        SunColorBuffer resolved;
        {
            ID3D12Resource *rb2 = buffer(D3D12_HEAP_TYPE_READBACK, sizeof(SunColorBuffer), D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            ID3D12GraphicsCommandList4_CopyBufferRegion(cl, rb2, 0, sun_color, 0, sizeof(SunColorBuffer));
            if (!run(cl))
                return 1;
            void *m = NULL;
            ID3D12Resource_Map(rb2, 0, NULL, &m);
            memcpy(&resolved, m, sizeof(resolved));
            ID3D12Resource_Unmap(rb2, 0, NULL);
            ID3D12Resource_Release(rb2);
        }
        const bool sun_ok = isfinite(resolved.sun_luminance) && isfinite(resolved.sky_luminance) &&
                            resolved.sun_luminance > 0.f && resolved.sky_luminance > 0.f;
        printf("  sun colour (%.4f %.4f %.4f) luminance %.4f, sky colour (%.4f %.4f %.4f) luminance %.4f%s\n",
               resolved.sun_color[0], resolved.sun_color[1], resolved.sun_color[2], resolved.sun_luminance,
               resolved.sky_color[0], resolved.sky_color[1], resolved.sky_color[2], resolved.sky_luminance,
               sun_ok ? "" : "  <-- FAIL");
        if (!sun_ok)
            sky_failed++;

        // the envmap's six layers: finite; the sky above lit and brighter than the
        // ground below (physical_sky.comp's layer 2 looks up, 3 down: its eyeVec
        // per layer, then (x, z, -y))
        {
            const int E = D3D12_SKY_ENVMAP_SIZE;
            float *face = malloc((size_t)E * E * 4 * sizeof(float));
            int env_nonfinite = 0;
            double face_lum[6];
            for (UINT f = 0; f < 6; f++) {
                if (!read_subresource(cl, ca, envmap, f, E, E, face))
                    return 1;
                double sum = 0;
                for (size_t i = 0; i < (size_t)E * E; i++) {
                    const float *px = face + i * 4;
                    if (!isfinite(px[0]) || !isfinite(px[1]) || !isfinite(px[2]))
                        env_nonfinite++;
                    else
                        sum += 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
                }
                face_lum[f] = sum / ((double)E * E);
            }
            free(face);
            const bool env_ok = env_nonfinite == 0 && face_lum[2] > 0.01 && face_lum[2] > face_lum[3];
            printf("  envmap: %d non-finite texels; mean luminance per layer %.4f %.4f %.4f (up) %.4f (down) %.4f %.4f%s\n",
                   env_nonfinite, face_lum[0], face_lum[1], face_lum[2], face_lum[3], face_lum[4], face_lum[5],
                   env_ok ? "" : "  <-- FAIL");
            if (!env_ok)
                sky_failed++;
        }

        // the frames: the sky in the misses, the resolved sun on the quads
        const double sun_nl = 1.0 / sqrt(0.6 * 0.6 + 1.0);
        const double expected_direct = 0.5 * resolved.sun_luminance * sun_nl / 3.14159265 * (1.0 - 0.04);
        const bool frames_ok = sky_nonfinite == 0 && sky_lum[0] > 0.02 && sky_lum[0] > 2.0 * skyless_misses &&
                               sky_lum[1] > 0.02 && sky_flat > 0.85 * expected_direct && sky_flat < 100.0 * expected_direct + 1.0 &&
                               static_lum[0] > 0.02 && static_flat_misses > 0.0;
        printf("  physical sky frames: TAA_OUTPUT misses %.3f (no sky: %.3f), quads %.3f; FLAT_COLOR quads %.3f HDR "
               "(the resolved sun alone %.3f)\n", sky_lum[0], skyless_misses, sky_lum[1], sky_flat, expected_direct);
        printf("  skybox frames: TAA_OUTPUT misses %.3f, quads %.3f; FLAT_COLOR misses %.3f HDR; %d non-finite in all%s\n",
               static_lum[0], static_lum[1], static_flat_misses, sky_nonfinite, frames_ok ? "" : "  <-- FAIL");
        if (!frames_ok)
            sky_failed++;
        printf("  %d validation errors in phase 3\n", errors - errors_before);
        for (int i = 0; i < NUM_FILES; i++)
            free(files[i]);
        free(grid_v);
        free(grid_i);
    }

    /* --------------------------------------------------------------------- */
    /* PHASE 4: reflect_refract                                               */
    /* --------------------------------------------------------------------- */
    /* A second world: the left quad (z 100) is a chrome mirror, the right one
       (z 200) stays regular, and a chrome wall stands behind the camera at
       z -100. The left pixels' ray d = (dx, dy, 1) is mirrored to the wall, at
       (300 dx, 300 dy, -100) - the first bounce - and from there back onto the
       mirror, at (500 dx, 500 dy, 100) - the second. reflect_refract leaves that
       surface in PT_SHADING_POSITION (and its material in .w) for the lighting;
       no bounce rays and no A-SVGF here, as both rewrite it. */
    int refl_failed = 0;
    {
        printf("\nphase 4: reflect_refract (passes.c), a chrome mirror facing a chrome wall\n");
        const int errors_before = errors;

        VboPrimitive prims2[12];
        int num_prims2 = 0;
        num_prims2 += add_quad(prims2 + num_prims2, -1000.f, 0.f, -1000.f, 0.f, 100.f, MATERIAL_KIND_CHROME | 1);
        num_prims2 += add_quad(prims2 + num_prims2, 0.f, 1000.f, -1000.f, 0.f, 200.f, MATERIAL_KIND_REGULAR | 2);
        num_prims2 += add_quad(prims2 + num_prims2, -1000.f, 1000.f, -1000.f, 1000.f, -100.f, MATERIAL_KIND_CHROME | 3);
        const uint64_t prims2_size = sizeof(VboPrimitive) * num_prims2;
        const uint64_t world2_size = prims2_size + (uint64_t)num_prims2 * 9 * sizeof(float);
        uint8_t *world2_data = calloc(1, world2_size);
        memcpy(world2_data, prims2, prims2_size);
        float *pos2 = (float *)(world2_data + prims2_size);
        for (int i = 0; i < num_prims2; i++) {
            memcpy(pos2 + i * 9 + 0, prims2[i].pos0, sizeof(vec3));
            memcpy(pos2 + i * 9 + 3, prims2[i].pos1, sizeof(vec3));
            memcpy(pos2 + i * 9 + 6, prims2[i].pos2, sizeof(vec3));
        }
        ID3D12Resource *world2 = buffer(D3D12_HEAP_TYPE_DEFAULT, world2_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *world2_up = upload(world2_data, world2_size);
        free(world2_data);

        d3d12_as_triangles_t geom2 = { ID3D12Resource_GetGPUVirtualAddress(world2) + prims2_size, (uint32_t)num_prims2 * 3, 0, 0 };
        d3d12_as_sizes_t bs2, ts2;
        if (!d3d12_blas_sizes(dev, &geom2, 1, bf, &bs2) || !d3d12_tlas_sizes(dev, 1, bf, &ts2)) {
            printf("FAIL: no AS sizes\n");
            return 1;
        }
        d3d12_as_pool_t pool2;
        if (!d3d12_as_pool_create(&pool2, (ID3D12Device *)dev, bs2.result + ts2.result + 2 * D3D12_AS_ALIGNMENT,
                                  L"phase 4 pool")) {
            printf("FAIL: AS pool\n");
            return 1;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS blas2 = d3d12_as_pool_alloc(&pool2, bs2.result);
        const D3D12_GPU_VIRTUAL_ADDRESS tlas2 = d3d12_as_pool_alloc(&pool2, ts2.result);
        ID3D12Resource *scratch2 = buffer(D3D12_HEAP_TYPE_DEFAULT, max(bs2.scratch, ts2.scratch),
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        D3D12_RAYTRACING_INSTANCE_DESC inst2 = inst;
        inst2.AccelerationStructure = blas2;
        ID3D12Resource *inst2_up = upload(&inst2, sizeof(inst2));

        ID3D12CommandAllocator_Reset(ca);
        ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, world2, 0, world2_up, 0, world2_size);
        transition(cl, world2, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d3d12_blas_build(cl, &geom2, 1, bf, blas2, ID3D12Resource_GetGPUVirtualAddress(scratch2), 0);
        d3d12_as_barrier(cl, &pool2);
        {
            D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
            b.UAV.pResource = scratch2;
            ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &b);
        }
        d3d12_tlas_build(cl, ID3D12Resource_GetGPUVirtualAddress(inst2_up), 1, bf, tlas2,
                         ID3D12Resource_GetGPUVirtualAddress(scratch2));
        d3d12_as_barrier(cl, &pool2);
        transition(cl, world2, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!run(cl)) {
            printf("FAIL: phase 4's world\n");
            return 1;
        }
        ID3D12Resource_Release(world2_up);
        ID3D12Resource_Release(inst2_up);
        // the GPU is idle (run waits): the sets are rewritten in place, as world.c's map load
        d3d12_set_write_buffer(&vb_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_WORLD, world2, 0, prims2_size);
        d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, tlas2);

        // TILE COMPACTION's resources, as world.c makes them: the lists for W x H
        // screen images (vkpt_pt_tile_layout's arithmetic), the reset copy (this
        // view's header in d3d12_tile_header's form, then zeroed masks), the copy of
        // a bounce's arguments, the DISPATCH command signature
        const uint32_t tile_rows = 3;
        const uint32_t ttx = (W / 2 + 7) / 8, tty = (H + 7) / 8, tper_field = ttx * tty;
        const uint32_t tmax = ((W + 7) / 8) * ((H + 7) / 8) * 2, tmask_stride = (tmax + 31) / 32;
        const uint32_t tdata = PT_TILE_HDR_WORDS + PT_TILE_LISTS * tmask_stride;
        const uint64_t tsize = (uint64_t)(tdata + PT_TILE_LISTS * tmax) * sizeof(uint32_t);
        uint32_t *treset = calloc(tdata, sizeof(uint32_t));
        treset[PT_TILE_HDR_TILES_X] = ttx;              // vkpt_pt_tile_header's
        treset[PT_TILE_HDR_PER_FIELD] = tper_field;
        treset[PT_TILE_HDR_MASK] = PT_TILE_HDR_WORDS;
        treset[PT_TILE_HDR_MASK_STRIDE] = tmask_stride;
        treset[PT_TILE_HDR_DATA] = tdata;
        treset[PT_TILE_HDR_DATA_STRIDE] = tmax;
        d3d12_tile_header(treset, tile_rows);
        ID3D12Resource *tile_buf = buffer(D3D12_HEAP_TYPE_DEFAULT, tsize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *tile_reset = upload(treset, (uint64_t)tdata * sizeof(uint32_t));
        ID3D12Resource *tile_args = buffer(D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_FLAG_NONE,
                                           D3D12_RESOURCE_STATE_COMMON);
        ID3D12CommandSignature *tile_sig = NULL;
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg;
            memset(&arg, 0, sizeof(arg));
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
            D3D12_COMMAND_SIGNATURE_DESC sd;
            memset(&sd, 0, sizeof(sd));
            sd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
            sd.NumArgumentDescs = 1;
            sd.pArgumentDescs = &arg;
            if (!tile_buf || !tile_reset || !tile_args ||
                FAILED(ID3D12Device5_CreateCommandSignature(dev, &sd, NULL, &IID_ID3D12CommandSignature,
                                                            (void **)&tile_sig))) {
                printf("FAIL: phase 4's tile lists\n");
                return 1;
            }
        }
        d3d12_set_write_buffer(&tiles_set, 0, 0, tile_buf, 0, tsize);
        // what each list must hold: the tiles with a launch whose surface is the
        // mirror (the primary rays' list 0), the wall it sees (bounce 0's list 1),
        // the mirror again (bounce 1's list 2) - the same tiles each time
        bool *mirror_tile = calloc(2 * tper_field, sizeof(bool));
        uint32_t mirror_tiles = 0;
        for (int z = 0; z < 2; z++)
            for (int ly = 0; ly < H; ly++)
                for (int lx = 0; lx < W / 2; lx++) {
                    const int sx = z == 0 ? lx * 2 + (ly & 1) : lx * 2 + 1 - (ly & 1);
                    const float dx = (((sx + 0.5f) / W) * 2.f - 1.f) * LIT_TAN_X;
                    const float dy = -(((ly + 0.5f) / H) * 2.f - 1.f) * LIT_TAN_Y;
                    const uint32_t id = z * tper_field + (ly >> 3) * ttx + (lx >> 3);
                    if (dy < 0 && dx < 0 && !mirror_tile[id]) {
                        mirror_tile[id] = true;
                        mirror_tiles++;
                    }
                }
        float *full_grid[2] = { NULL, NULL };   // PT_SHADING_POSITION without lists, per bounce count

        static const struct { int bounces, frames; bool tiles; } refl_configs[] = {
            { 1, 3, false }, { 2, 3, false }, { 1, 3, true }, { 2, 3, true },
        };
        for (size_t c = 0; c < sizeof(refl_configs) / sizeof(refl_configs[0]); c++) {
            const int config_errors = errors;
            const int n = refl_configs[c].bounces;
            for (int k = 0; k < refl_configs[c].frames; k++, frame++) {
                const int odd = frame & 1;
                {
                    void *p = NULL;
                    ID3D12Resource_Map(ubo_buf, 0, NULL, &p);
                    QVKUniformBuffer_t *u = (QVKUniformBuffer_t *)p;
                    lit_ubo(u, frame, false, 0.f, false, taa_w, taa_h);
                    u->pt_reflect_refract = (float)n;
                    // Untextured surfaces are pinned at roughness 1, and primary_rays
                    // turns chrome rougher than MAX_MIRROR_ROUGHNESS into a regular
                    // surface: only the global override makes a mirror of it.
                    u->pt_roughness_override = 0.f;
                    // TAAU (the game's real-time mode) samples at sub_pixel_jitter,
                    // zero here: exact pixel centres. TAA would jitter every ray.
                    u->flt_taa = AA_MODE_UPSCALE;
                    ID3D12Resource_Unmap(ubo_buf, 0, NULL);
                }
                ID3D12CommandAllocator_Reset(ca);
                ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
                ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
                for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
                    if (i == frame % NUM_LIGHT_STATS_BUFFERS)
                        d3d12_clear_buffer(cl, stats[i], stats_size, zeros);
                const d3d12_passes_t p = {
                    .cmd = cl,
                    .enhanced_barriers = enhanced,
                    .psos = psos,
                    .pt_sets = { &rt_set, &ubo_set, &textures[odd], &vb_set, &tiles_set },
                    .standard_sets = { &ubo_set, &textures[odd], &vb_set },
                    .tonemap_buffer = tonemap,
                    .tonemap_size = sizeof(ToneMappingBuffer),
                    .zero_buffer = zeros,
                    .tile_buffer = refl_configs[c].tiles ? tile_buf : NULL,
                    .tile_reset = tile_reset,
                    .tile_reset_size = tdata * (uint32_t)sizeof(uint32_t),
                    .tile_args = tile_args,
                    .tile_dispatch = tile_sig,
                };
                const d3d12_passes_frame_t pf = {
                    .field_width = W / 2, .slice_width = W, .render_width = W, .render_height = H,
                    .taa_output_width = W, .taa_output_height = H, .taa_image_width = taa_w, .taa_image_height = taa_h,
                    .enable_denoiser = false, .reflect_refract = n, .num_bounce_rays = 0.f,
                    .tone_mapping = true, .tm_reset = k == 0, .frame_time = 1.f / 60.f,
                    .tm_slope_blur_sigma = 6.f, .tm_knee_start = 0.9f, .tm_white_point = 10.f,
                };
                d3d12_record_passes(&p, &pf);
                if (!run(cl)) {
                    printf("FAIL: phase 4 frame %d did not complete (device %s)\n", k,
                           SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
                    return 1;
                }
            }
            const int last_odd = (frame - 1) & 1;

            // the surfaces reflect_refract left, launch by launch as phase 1 reads them
            if (!read_image(cl, ca, img[VKPT_IMG_PT_SHADING_POSITION], W, H, pixels))
                return 1;
            int wrong = 0, mirrored = 0;
            for (int z = 0; z < 2; z++)
                for (int ly = 0; ly < H; ly++)
                    for (int lx = 0; lx < W / 2; lx++) {
                        const int ix = lx + z * (W / 2);
                        const int sx = z == 0 ? lx * 2 + (ly & 1) : lx * 2 + 1 - (ly & 1);
                        // lit_ubo's camera (projection(): y up the screen), not phase 1's
                        const float dx = (((sx + 0.5f) / W) * 2.f - 1.f) * LIT_TAN_X;
                        const float dy = -(((ly + 0.5f) / H) * 2.f - 1.f) * LIT_TAN_Y;
                        float e[3] = { 0, 0, 0 };
                        uint32_t emat = 0;
                        if (dy < 0 && dx < 0) {         // the mirror, then the wall, then the mirror
                            const float t = n == 1 ? 300.f : 500.f;
                            e[0] = dx * t; e[1] = dy * t; e[2] = n == 1 ? -100.f : 100.f;
                            emat = n == 1 ? 3 : 1;
                            mirrored++;
                        } else if (dy < 0) {            // the regular quad
                            e[0] = dx * 200.f; e[1] = dy * 200.f; e[2] = 200.f;
                            emat = 2;
                        }
                        // PT_VIEW_DIRECTION is rgba16f: each bounce leaves its direction
                        // there at 2^-11 relative precision for the next, so a mirrored
                        // point drifts by about 1/1000 of the path after the mirror
                        // (200 units a bounce). Getting the reflection wrong costs hundreds.
                        const float tol = (dy < 0 && dx < 0) ? 0.05f + 0.002f * 200.f * n : 0.1f;
                        const float *px = pixels + ((size_t)ly * W + ix) * 4;
                        uint32_t mat;
                        memcpy(&mat, &px[3], 4);
                        bool ok = (mat & MATERIAL_INDEX_MASK) == emat;
                        if (emat)
                            ok = ok && fabsf(px[0] - e[0]) < tol && fabsf(px[1] - e[1]) < tol && fabsf(px[2] - e[2]) < tol;
                        if (!ok && wrong++ < 8)
                            printf("    pixel (%d,%d) field %d: got (%.2f %.2f %.2f) material 0x%08x, expected "
                                   "(%.2f %.2f %.2f) material %u\n", sx, ly, z, px[0], px[1], px[2], mat, e[0], e[1],
                                   e[2], emat);
                    }

            // over tile lists: the full grid's surfaces to the bit, and each list the
            // mirror's tiles - list l was filled by the primary rays (0) or bounce l - 1,
            // so lists 0..n - its row count ceil(tiles / 3), the rest empty
            int differ = 0;
            bool lists_ok = true;
            char lists_note[128] = "";
            if (!refl_configs[c].tiles) {
                full_grid[n - 1] = malloc((size_t)W * H * 4 * sizeof(float));
                memcpy(full_grid[n - 1], pixels, (size_t)W * H * 4 * sizeof(float));
            } else {
                for (size_t i = 0; i < (size_t)W * H * 4; i++)
                    differ += !full_grid[n - 1] || memcmp(&pixels[i], &full_grid[n - 1][i], sizeof(float)) != 0;
                uint32_t head[PT_TILE_LISTS * 4];
                if (!read_buffer(cl, ca, tile_buf, 0, sizeof(head), head))
                    return 1;
                int at = 0;
                for (int l = 0; l < PT_TILE_LISTS; l++) {
                    const uint32_t want = l <= n ? mirror_tiles : 0;
                    const uint32_t *a = head + l * 4;
                    lists_ok = lists_ok && a[0] == want && a[1] == tile_rows &&
                               a[2] == (want + tile_rows - 1) / tile_rows && a[3] == 1;
                    at += snprintf(lists_note + at, sizeof(lists_note) - (size_t)at, "%s%u/%u", l ? ", " : "",
                                   a[0], a[2]);
                }
            }

            // and the picture: finite, the mirror's pixels lit
            int nonfinite = 0;
            if (!read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_TAA_OUTPUT, last_odd)], W, H, pixels))
                return 1;
            for (size_t i = 0; i < (size_t)W * H * 4; i++)
                nonfinite += !isfinite(pixels[i]);
            const bool ok = wrong == 0 && nonfinite == 0 && errors == config_errors && differ == 0 && lists_ok;
            printf("  pt_reflect_refract %d%s, %d frames: %d of %d launches wrong (%d through the mirror), %d non-finite, "
                   "%d validation errors%s\n", n, refl_configs[c].tiles ? " over tile lists" : "", refl_configs[c].frames,
                   wrong, W * H, mirrored, nonfinite, errors - config_errors, ok ? "" : "  <-- FAIL");
            if (refl_configs[c].tiles)
                printf("    lists (tiles/rows) %s, against %u mirror tiles in rows of %u; %d of %d floats of "
                       "PT_SHADING_POSITION differ from the full grid's%s\n", lists_note, mirror_tiles, tile_rows,
                       differ, W * H * 4, differ == 0 && lists_ok ? "" : "  <-- FAIL");
            if (!ok)
                refl_failed++;
        }
        printf("  %d validation errors in phase 4\n", errors - errors_before);
    }

    /* --------------------------------------------------------------------- */
    /* PHASE 5: dynamic models                                                */
    /* --------------------------------------------------------------------- */
    /* The model: a 120 x 60 quad off its centre (x -40..80, y -30..30), frame 0
       at z 0 and frame 1 at z 40. Its instance draws frame 1 with backlerp 0.25
       (z 30), turned a quarter about z (x -> +y, y -> -x) and moved by
       (0, 70, 120): in the world it covers x -30..30, y 30..150 at z 150 - the
       upper half of the view, where phase 1's world has nothing. A transposed
       matrix or a wrong lerp moves it. Last frame it was at backlerp 0.5 and
       10 nearer (transform_prev): its motion is -20 in z. */
    int dyn_failed = 0;
    {
        printf("\nphase 5: dynamic models (instance_geometry, animate_materials, accel.c's per-frame BLASes)\n");
        const int errors_before = errors;

        VboPrimitive model_prims[8];
        add_quad(model_prims + 0, -40.f, 80.f, -30.f, 30.f, 0.f, MATERIAL_KIND_REGULAR | 1);
        add_quad(model_prims + 4, -40.f, 80.f, -30.f, 30.f, 40.f, MATERIAL_KIND_REGULAR | 1);
        static const float T[16] = { 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 70, 120, 1 };
        static const float T_prev[16] = { 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 70, 110, 1 };
        const float lerp = 0.25f, lerp_prev = 0.5f, model_z = 150.f;
        // the model's run of the instanced buffers starts past 0, as
        // transparent_prim_offset's and masked_prim_offset's do
        const uint32_t render_offset = 8, inst_prims = 16;

        // fill_model_instance's fields; the instance's material overrides its primitives'
        memset(ib, 0, sizeof(InstanceBuffer));
        ModelInstance *mi = &ib->model_instances[0];
        memcpy(mi->transform, T, sizeof(T));
        memcpy(mi->transform_prev, T_prev, sizeof(T_prev));
        mi->material = MATERIAL_KIND_REGULAR | 4;
        mi->cluster = -1;
        mi->source_buffer_idx = VERTEX_BUFFER_FIRST_MODEL;     // model slot 0
        mi->prim_count = 4;
        mi->prim_offset_curr_pose_curr_frame = 4;              // frame 1
        mi->prim_offset_prev_pose_curr_frame = 0;              // oldframe 0
        mi->prim_offset_curr_pose_prev_frame = 4;
        mi->prim_offset_prev_pose_prev_frame = 0;
        mi->pose_lerp_curr_frame = lerp;
        mi->pose_lerp_prev_frame = lerp_prev;
        mi->iqm_matrix_offset_curr_frame = -1;                 // vertex animation, no skeleton
        mi->iqm_matrix_offset_prev_frame = -1;
        mi->frame = 0;
        mi->alpha = 1.f;
        mi->render_buffer_idx = VERTEX_BUFFER_INSTANCED;
        mi->render_prim_offset = render_offset;
        ib->animated_model_indices[0] = 0;
        // TLAS instance 0 is phase 1's world, 1 the model's section - as append_blas writes it
        ib->tlas_instance_model_indices[0] = -1;
        ib->tlas_instance_prim_offsets[0] = 0;
        ib->tlas_instance_model_indices[1] = -1;
        ib->tlas_instance_prim_offsets[1] = render_offset;

        // the animated world: two-frame materials 5 <-> 6 (flags kept), a
        // one-frame 1, and a 5 past num_static_primitives that must stay
        VboPrimitive anim_prims[4];
        memset(anim_prims, 0, sizeof(anim_prims));
        const uint32_t anim_in[4] = { MATERIAL_KIND_REGULAR | 5, MATERIAL_KIND_REGULAR | MATERIAL_FLAG_LIGHT | 6,
                                      MATERIAL_KIND_REGULAR | 1, MATERIAL_KIND_REGULAR | 5 };
        const uint32_t anim_out[4] = { MATERIAL_KIND_REGULAR | 6, MATERIAL_KIND_REGULAR | MATERIAL_FLAG_LIGHT | 5,
                                       MATERIAL_KIND_REGULAR | 1, MATERIAL_KIND_REGULAR | 5 };
        for (int i = 0; i < 4; i++)
            anim_prims[i].material_id = anim_in[i];

        ID3D12Resource *model_buf = buffer(D3D12_HEAP_TYPE_DEFAULT, sizeof(model_prims),
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *anim_buf = buffer(D3D12_HEAP_TYPE_DEFAULT, sizeof(anim_prims),
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *prims_inst = buffer(D3D12_HEAP_TYPE_DEFAULT, inst_prims * sizeof(VboPrimitive),
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *pos_inst = buffer(D3D12_HEAP_TYPE_DEFAULT, inst_prims * 9 * sizeof(float),
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *model_up = upload(model_prims, sizeof(model_prims));
        ID3D12Resource *anim_up = upload(anim_prims, sizeof(anim_prims));
        ID3D12Resource *ib_up = upload(ib, sizeof(InstanceBuffer));
        const D3D12_GPU_VIRTUAL_ADDRESS pos_va = ID3D12Resource_GetGPUVirtualAddress(pos_inst);

        // the GPU is idle: world.c's descriptors, as d3d12_world_models_commit and
        // d3d12_world_set_primbuf_size write them; the animated world in WORLD for now
        d3d12_set_write_buffer(&vb_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_FIRST_MODEL, model_buf, 0,
                               sizeof(model_prims));
        d3d12_set_write_buffer(&vb_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_INSTANCED, prims_inst, 0,
                               inst_prims * sizeof(VboPrimitive));
        d3d12_set_write_buffer(&vb_set, POSITION_BUFFER_BINDING_IDX, 0, pos_inst, 0, inst_prims * 9 * sizeof(float));
        d3d12_set_write_buffer(&vb_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_WORLD, anim_buf, 0, sizeof(anim_prims));

        // list 1: the uploads, then animate_materials alone over 3 of the 4
        {
            void *mp = NULL;
            ID3D12Resource_Map(ubo_buf, 0, NULL, &mp);
            ((QVKUniformBuffer_t *)mp)->num_static_primitives = 3;
            ID3D12Resource_Unmap(ubo_buf, 0, NULL);
        }
        ID3D12CommandAllocator_Reset(ca);
        ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, model_buf, 0, model_up, 0, sizeof(model_prims));
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, anim_buf, 0, anim_up, 0, sizeof(anim_prims));
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, instance_buffer, 0, ib_up, 0, sizeof(InstanceBuffer));
        transition(cl, model_buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cl, anim_buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cl, instance_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
        {
            const d3d12_passes_t p = {
                .cmd = cl, .enhanced_barriers = enhanced, .psos = psos,
                .pt_sets = { &rt_set, &ubo_set, &textures[0], &vb_set, &tiles_set },
                .standard_sets = { &ubo_set, &textures[0], &vb_set },
                .tonemap_buffer = tonemap, .tonemap_size = sizeof(ToneMappingBuffer), .zero_buffer = zeros,
            };
            d3d12_record_instance_geometry(&p, 0, 3);
        }
        if (!run(cl)) {
            printf("FAIL: phase 5's animate_materials did not complete\n");
            return 1;
        }
        ID3D12Resource_Release(model_up);
        ID3D12Resource_Release(anim_up);
        ID3D12Resource_Release(ib_up);
        VboPrimitive anim_got[4];
        if (!read_buffer(cl, ca, anim_buf, 0, sizeof(anim_got), anim_got))
            return 1;
        int anim_wrong = 0;
        for (int i = 0; i < 4; i++)
            if (anim_got[i].material_id != anim_out[i]) {
                anim_wrong++;
                printf("    world primitive %d: material 0x%08x, expected 0x%08x\n", i, anim_got[i].material_id,
                       anim_out[i]);
            }
        printf("  animate_materials: %d of 4 world primitives wrong%s\n", anim_wrong, anim_wrong ? "  <-- FAIL" : "");

        // phase 1's world back in WORLD (its BLAS is still in phase 1's pool)
        d3d12_set_write_buffer(&vb_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_WORLD, world, 0, primitives_size);

        // the TLAS of two instances, and its instance list (rewritten every frame,
        // as world.c's upload slice is)
        d3d12_as_sizes_t ts5;
        if (!d3d12_tlas_sizes(dev, 2, bf, &ts5)) {
            printf("FAIL: no TLAS sizes\n");
            return 1;
        }
        d3d12_as_pool_t pool5;
        if (!d3d12_as_pool_create(&pool5, (ID3D12Device *)dev, ts5.result + D3D12_AS_ALIGNMENT, L"phase 5 TLAS")) {
            printf("FAIL: AS pool\n");
            return 1;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS tlas5 = d3d12_as_pool_alloc(&pool5, ts5.result);
        ID3D12Resource *scratch5 = buffer(D3D12_HEAP_TYPE_DEFAULT, ts5.scratch, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *inst5_up = upload(NULL, 2 * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, tlas5);

        // the per-frame BLASes as world.c plans them: an empty opaque section, then
        // the model's - pt_blas_fast_trace 0's build
        d3d12_dynamic_as_t dyn;
        memset(&dyn, 0, sizeof(dyn));
        d3d12_dynamic_blas_t sections[2];
        memset(sections, 0, sizeof(sections));
        sections[0].geometry = d3d12_triangle_run(pos_va, 0, 0);
        sections[1].geometry = d3d12_triangle_run(pos_va, render_offset, 4);
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS dyn_flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

        ID3D12Resource *first_pool = NULL;
        int plan_wrong = 0;
        for (int k = 0; k < 2; k++, frame++) {
            const int odd = frame & 1;
            // the plan first: the instance list names its addresses
            if (!d3d12_dynamic_blas_plan(dev, &dyn, sections, 2, dyn_flags) || sections[0].address ||
                !sections[1].address) {
                printf("FAIL: phase 5's BLAS plan (the empty section at %llx, the model's at %llx)\n",
                       (unsigned long long)sections[0].address, (unsigned long long)sections[1].address);
                return 1;
            }
            // the same sizes next frame: the same memory, not made again
            if (k == 0)
                first_pool = dyn.pool.buffer;
            else if (dyn.pool.buffer != first_pool)
                plan_wrong++;

            D3D12_RAYTRACING_INSTANCE_DESC *descs = NULL;
            ID3D12Resource_Map(inst5_up, 0, NULL, (void **)&descs);
            descs[0] = inst;
            memset(&descs[1], 0, sizeof(descs[1]));
            descs[1].Transform[0][0] = descs[1].Transform[1][1] = descs[1].Transform[2][2] = 1.f;
            descs[1].InstanceID = VERTEX_BUFFER_INSTANCED;
            descs[1].InstanceMask = AS_FLAG_OPAQUE;
            descs[1].InstanceContributionToHitGroupIndex = SBTO_OPAQUE;
            descs[1].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            descs[1].AccelerationStructure = sections[1].address;
            ID3D12Resource_Unmap(inst5_up, 0, NULL);
            {
                void *mp = NULL;
                ID3D12Resource_Map(ubo_buf, 0, NULL, &mp);
                QVKUniformBuffer_t *u = (QVKUniformBuffer_t *)mp;
                lit_ubo(u, frame, false, 0.f, false, taa_w, taa_h);
                u->pt_reflect_refract = 0.f;
                u->flt_taa = AA_MODE_UPSCALE;   // exact pixel centres, as phase 4
                ID3D12Resource_Unmap(ubo_buf, 0, NULL);
            }

            // world.c's order: instance_geometry, the per-frame BLASes, the TLAS, the passes
            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
            for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
                if (i == frame % NUM_LIGHT_STATS_BUFFERS)
                    d3d12_clear_buffer(cl, stats[i], stats_size, zeros);
            const d3d12_passes_t p = {
                .cmd = cl,
                .enhanced_barriers = enhanced,
                .psos = psos,
                .pt_sets = { &rt_set, &ubo_set, &textures[odd], &vb_set, &tiles_set },
                .standard_sets = { &ubo_set, &textures[odd], &vb_set },
                .tonemap_buffer = tonemap,
                .tonemap_size = sizeof(ToneMappingBuffer),
                .zero_buffer = zeros,
            };
            transition(cl, pos_inst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            d3d12_record_instance_geometry(&p, 1, 0);
            transition(cl, pos_inst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            d3d12_dynamic_blas_build(cl, &dyn, sections, 2, dyn_flags);
            d3d12_tlas_build(cl, ID3D12Resource_GetGPUVirtualAddress(inst5_up), 2, bf, tlas5,
                             ID3D12Resource_GetGPUVirtualAddress(scratch5));
            d3d12_as_barrier(cl, &pool5);
            const d3d12_passes_frame_t pf = {
                .field_width = W / 2, .slice_width = W, .render_width = W, .render_height = H,
                .taa_output_width = W, .taa_output_height = H, .taa_image_width = taa_w, .taa_image_height = taa_h,
                .enable_denoiser = false, .reflect_refract = 0, .num_bounce_rays = 0.f,
                .tone_mapping = true, .tm_reset = k == 0, .frame_time = 1.f / 60.f,
                .tm_slope_blur_sigma = 6.f, .tm_knee_start = 0.9f, .tm_white_point = 10.f,
            };
            d3d12_record_passes(&p, &pf);
            if (!run(cl)) {
                printf("FAIL: phase 5 frame %d did not complete (device %s)\n", k,
                       SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
                return 1;
            }
        }
        const int last_odd = (frame - 1) & 1;
        printf("  the BLAS plan: %s\n", plan_wrong ? "made its memory again for the same sizes  <-- FAIL" :
                                                     "the empty section none, the model's placed, the memory kept");

        // instance_geometry's output: the instanced positions and primitives
        float pos_got[4 * 9];
        VboPrimitive prims_got[4];
        if (!read_buffer(cl, ca, pos_inst, (uint64_t)render_offset * 9 * sizeof(float), sizeof(pos_got), pos_got) ||
            !read_buffer(cl, ca, prims_inst, (uint64_t)render_offset * sizeof(VboPrimitive), sizeof(prims_got), prims_got))
            return 1;
        int geo_wrong = 0;
        for (int t = 0; t < 4; t++) {
            const VboPrimitive *curr = &model_prims[4 + t], *prev = &model_prims[t];
            const float *vc[3] = { curr->pos0, curr->pos1, curr->pos2 };
            const float *vp[3] = { prev->pos0, prev->pos1, prev->pos2 };
            const VboPrimitive *g = &prims_got[t];
            const float *gp[3] = { g->pos0, g->pos1, g->pos2 };
            const uint32_t *motion[3] = { g->custom0, g->custom1, g->custom2 };
            for (int v = 0; v < 3; v++) {
                float e[3], e_prev[3];
                model_vertex(vc[v], vp[v], lerp, T, e);
                model_vertex(vc[v], vp[v], lerp_prev, T_prev, e_prev);
                const float m[3] = { half_to_float((uint16_t)(motion[v][0] & 0xffff)),
                                     half_to_float((uint16_t)(motion[v][0] >> 16)),
                                     half_to_float((uint16_t)(motion[v][1] & 0xffff)) };
                const float *pp = pos_got + t * 9 + v * 3;
                bool ok = true;
                for (int c = 0; c < 3; c++)
                    ok = ok && fabsf(pp[c] - e[c]) < 1e-3f && fabsf(gp[v][c] - e[c]) < 1e-3f &&
                         fabsf(m[c] - (e_prev[c] - e[c])) < 0.02f;
                if (!ok && geo_wrong++ < 8)
                    printf("    triangle %d vertex %d: position (%.3f %.3f %.3f), primitive (%.3f %.3f %.3f), motion "
                           "(%.3f %.3f %.3f); expected (%.3f %.3f %.3f), motion (%.3f %.3f %.3f)\n", t, v, pp[0], pp[1],
                           pp[2], gp[v][0], gp[v][1], gp[v][2], m[0], m[1], m[2], e[0], e[1], e[2], e_prev[0] - e[0],
                           e_prev[1] - e[1], e_prev[2] - e[2]);
            }
            if (g->material_id != (MATERIAL_KIND_REGULAR | 4) || g->instance != 0 || g->cluster != -1 ||
                g->emissive_and_alpha != 0x3c003c00u) {
                if (geo_wrong++ < 8)
                    printf("    triangle %d: material 0x%08x instance %u cluster %d emissive/alpha 0x%08x; expected "
                           "0x%08x 0 -1 0x3c003c00\n", t, g->material_id, g->instance, g->cluster, g->emissive_and_alpha,
                           MATERIAL_KIND_REGULAR | 4);
            }
        }
        printf("  instance_geometry: %d wrong of 4 triangles' positions, motion and fields%s\n", geo_wrong,
               geo_wrong ? "  <-- FAIL" : "");

        // the primary rays: phase 1's world below, the model above
        if (!read_image(cl, ca, img[VKPT_IMG_PT_SHADING_POSITION], W, H, pixels))
            return 1;
        int wrong = 0, on_model = 0, edge = 0;
        for (int z = 0; z < 2; z++)
            for (int ly = 0; ly < H; ly++)
                for (int lx = 0; lx < W / 2; lx++) {
                    const int ix = lx + z * (W / 2);
                    const int sx = z == 0 ? lx * 2 + (ly & 1) : lx * 2 + 1 - (ly & 1);
                    const float dx = (((sx + 0.5f) / W) * 2.f - 1.f) * LIT_TAN_X;
                    const float dy = -(((ly + 0.5f) / H) * 2.f - 1.f) * LIT_TAN_Y;
                    float e[3] = { 0, 0, 0 };
                    uint32_t emat = 0;
                    const float hx = dx * model_z, hy = dy * model_z;
                    if (dy < 0) {                   // phase 1's quads
                        const float qz = dx < 0 ? 100.f : 200.f;
                        e[0] = dx * qz; e[1] = dy * qz; e[2] = qz;
                        emat = dx < 0 ? 1 : 2;
                    } else {
                        // a launch whose ray passes within a quarter unit of the
                        // model's outline is not called either way
                        const float margin = fminf(fminf(fabsf(hx + 30.f), fabsf(hx - 30.f)),
                                                   fminf(fabsf(hy - 30.f), fabsf(hy - 150.f)));
                        if (margin < 0.25f) {
                            edge++;
                            continue;
                        }
                        if (hx > -30.f && hx < 30.f && hy > 30.f && hy < 150.f) {
                            e[0] = hx; e[1] = hy; e[2] = model_z;
                            emat = 4;
                            on_model++;
                        }
                    }
                    const float *px = pixels + ((size_t)ly * W + ix) * 4;
                    uint32_t mat;
                    memcpy(&mat, &px[3], 4);
                    bool ok = (mat & MATERIAL_INDEX_MASK) == emat;
                    if (emat)
                        ok = ok && fabsf(px[0] - e[0]) < 0.05f && fabsf(px[1] - e[1]) < 0.05f && fabsf(px[2] - e[2]) < 0.05f;
                    if (!ok && wrong++ < 8)
                        printf("    pixel (%d,%d) field %d: got (%.2f %.2f %.2f) material 0x%08x, expected "
                               "(%.2f %.2f %.2f) material %u\n", sx, ly, z, px[0], px[1], px[2], mat, e[0], e[1],
                               e[2], emat);
                }

        int nonfinite = 0;
        if (!read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_TAA_OUTPUT, last_odd)], W, H, pixels))
            return 1;
        for (size_t i = 0; i < (size_t)W * H * 4; i++)
            nonfinite += !isfinite(pixels[i]);
        const bool rays_ok = wrong == 0 && on_model > 0 && nonfinite == 0;
        printf("  primary rays: %d of %d launches wrong, %d on the model, %d too near its outline to call; "
               "%d non-finite%s\n", wrong, W * H, on_model, edge, nonfinite, rays_ok ? "" : "  <-- FAIL");

        d3d12_dynamic_as_destroy(&dyn);
        d3d12_as_pool_destroy(&pool5);
        ID3D12Resource_Release(scratch5);
        ID3D12Resource_Release(inst5_up);
        const int phase_errors = errors - errors_before;
        printf("  %d validation errors in phase 5\n", phase_errors);
        if (anim_wrong || plan_wrong || geo_wrong || !rays_ok || phase_errors)
            dyn_failed = 1;
    }

    /* --------------------------------------------------------------------- */
    /* PHASE 6: the effects TLAS                                              */
    /* --------------------------------------------------------------------- */
    /* world.c's transparency path: a particle, a sprite, a beam and an
       explosion, one to each quarter of the lower half of the view, 50 units
       out - in front of phase 1's world. Their buffers are filled as
       d3d12_world_render fills them (one staging slice laid out as
       transparency.c lays its frame out, each stream copied to the start of
       its own buffer), their BLASes planned and built as d3d12_world_plan_dynamic
       lays them out (the explosion a run of the instanced positions, the
       particle's and the sprite's quads through the one index list - the
       sprite's from its vertex offset - and the beam a box), and the effects
       TLAS built from the instance list after the geometry's, as world.c's
       second build. trace_effects_ray then composites each over the world into
       PT_TRANSPARENT: each effect's colour where it is, nothing anywhere else. */
#define FX_ALIGN(x) (((x) + (uint64_t)D3D12_AS_ALIGNMENT - 1) & ~((uint64_t)D3D12_AS_ALIGNMENT - 1))
    int fx_failed = 0;
    {
        printf("\nphase 6: the effects TLAS (a particle, a sprite, a beam, an explosion)\n");
        const int errors_before = errors;
        const float fz = 50.f;
        // world rectangles at z 50: x0, x1, y0, y1
        static const float particle_r[4] = { -46.f, -29.f, -21.f, -4.f };
        static const float sprite_r[4]   = { -21.f, -4.f, -21.f, -4.f };
        static const float explo_r[4]    = { 29.f, 46.f, -21.f, -4.f };
        const float beam_x = 12.5f, beam_y0 = -21.f, beam_y1 = -4.f, beam_r = 3.f;
        // the explosions' run starts past 0: the instance's custom index says where
        const uint32_t explo_offset = 4, inst6_prims = 8;

        // list 1: two 1x1 textures in the bindless block (1 blue: the sprite's;
        // 2 white: material 7's, the explosion's), material 7, the index list
        ID3D12CommandAllocator_Reset(ca);
        ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
        static const uint32_t texel[2] = { 0xffff0000u, 0xffffffffu };     // RGBA8: blue, white
        ID3D12Resource *tex6[2] = { NULL, NULL }, *tex6_up[2] = { NULL, NULL };
        for (int i = 0; i < 2; i++) {
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd = { 0 };
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 1; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
            if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                             (void **)&tex6[i]))) {
                printf("FAIL: phase 6's textures\n");
                return 1;
            }
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
            UINT64 total = 0;
            ID3D12Device5_GetCopyableFootprints(dev, &rd, 0, 1, 0, &fp, NULL, NULL, &total);
            tex6_up[i] = upload(NULL, total);
            uint8_t *map = NULL;
            ID3D12Resource_Map(tex6_up[i], 0, NULL, (void **)&map);
            memcpy(map + fp.Offset, &texel[i], 4);
            ID3D12Resource_Unmap(tex6_up[i], 0, NULL);
            D3D12_TEXTURE_COPY_LOCATION to = { 0 }, from = { 0 };
            to.pResource = tex6[i]; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = 0;
            from.pResource = tex6_up[i]; from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = fp;
            ID3D12GraphicsCommandList4_CopyTextureRegion(cl, &to, 0, 0, 0, &from, NULL);
            transition(cl, tex6[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = d3d12_heaps.resource_cpu;
            h.ptr += (SIZE_T)(1 + i) * d3d12_heaps.resource_increment;
            ID3D12Device5_CreateShaderResourceView(dev, tex6[i], &sv, h);
        }
        uint32_t mat7[MATERIAL_UINTS];
        memset(mat7, 0, sizeof(mat7));
        mat7[0] = 2u;                               // base_texture: the white one
        mat7[2] = 0x3c00u | (0xbc00u << 16);
        mat7[4] = 1u | (7u << 16);
        mat7[5] = 0x3c00u | (0x3800u << 16);
        ID3D12Resource *mat7_up = upload(mat7, sizeof(mat7));
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, light_buffer,
                                                    offsetof(LightBuffer, material_table) + 7 * MATERIAL_UINTS * 4,
                                                    mat7_up, 0, sizeof(mat7));
        transition(cl, light_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // fill_index_buffer's quad, uploaded once as d3d12_world_transparency_init does
        static const uint16_t quad_indices[8] = { 0, 1, 2, 2, 3, 0, 0, 0 };
        ID3D12Resource *idx6 = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *idx6_up = upload(quad_indices, sizeof(quad_indices));
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, idx6, 0, idx6_up, 0, sizeof(quad_indices));

        // the explosion: a double-wound quad of material 7 in the instanced
        // buffers at explo_offset, as instance_geometry would have left it
        VboPrimitive explo_prims[4];
        add_quad(explo_prims, explo_r[0], explo_r[1], explo_r[2], explo_r[3], fz, MATERIAL_KIND_REGULAR | 7);
        float explo_pos[4 * 9];
        for (int t = 0; t < 4; t++) {
            memcpy(explo_pos + t * 9 + 0, explo_prims[t].pos0, sizeof(vec3));
            memcpy(explo_pos + t * 9 + 3, explo_prims[t].pos1, sizeof(vec3));
            memcpy(explo_pos + t * 9 + 6, explo_prims[t].pos2, sizeof(vec3));
        }
        ID3D12Resource *prims6 = buffer(D3D12_HEAP_TYPE_DEFAULT, inst6_prims * sizeof(VboPrimitive),
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *pos6 = buffer(D3D12_HEAP_TYPE_DEFAULT, inst6_prims * 9 * sizeof(float),
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource *prims6_up = upload(explo_prims, sizeof(explo_prims));
        ID3D12Resource *pos6_up = upload(explo_pos, sizeof(explo_pos));
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, prims6, (uint64_t)explo_offset * sizeof(VboPrimitive), prims6_up, 0,
                                                    sizeof(explo_prims));
        ID3D12GraphicsCommandList4_CopyBufferRegion(cl, pos6, (uint64_t)explo_offset * 9 * sizeof(float), pos6_up, 0,
                                                    sizeof(explo_pos));
        if (!run(cl)) {
            printf("FAIL: phase 6's uploads did not complete\n");
            return 1;
        }
        for (int i = 0; i < 2; i++)
            ID3D12Resource_Release(tex6_up[i]);
        ID3D12Resource_Release(mat7_up);
        ID3D12Resource_Release(idx6_up);
        ID3D12Resource_Release(prims6_up);
        ID3D12Resource_Release(pos6_up);
        // the GPU is idle: the instanced buffers' descriptors, as d3d12_world_set_primbuf_size
        d3d12_set_write_buffer(&vb_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_INSTANCED, prims6, 0,
                               inst6_prims * sizeof(VboPrimitive));
        d3d12_set_write_buffer(&vb_set, POSITION_BUFFER_BINDING_IDX, 0, pos6, 0, inst6_prims * 9 * sizeof(float));

        // the frame's transparency, in transparency.c's order: the quads'
        // vertices (the particle's, then the sprite's), particle colours,
        // sprite infos, beam boxes, beam colours, beam intersections
        enum { TR_VERT, TR_AABB, TR_PCOL, TR_BCOL, TR_SINFO, TR_BINT, TR_STREAMS };
        const uint64_t tr_off[TR_STREAMS] = { 0, 128, 96, 152, 112, 168 };
        const uint64_t tr_size[TR_STREAMS] = { 8 * 12, 24, 16, 16, 16, 48 };
        const uint64_t tr_upload = 216, tr_capacity = 256, sprite_vertex_offset = 4 * 12;
        uint8_t tr_frame[256];
        memset(tr_frame, 0, sizeof(tr_frame));
        {
            float *v = (float *)(tr_frame + tr_off[TR_VERT]);
            const float *rects[2] = { particle_r, sprite_r };
            for (int q = 0; q < 2; q++) {
                const float *r = rects[q];
                const float c[4][3] = { { r[0], r[2], fz }, { r[1], r[2], fz }, { r[1], r[3], fz }, { r[0], r[3], fz } };
                memcpy(v + q * 12, c, sizeof(c));
            }
            const float particle_color[4] = { 1.f, 0.f, 0.f, 1.f }, beam_color[4] = { 0.f, 1.f, 0.f, 1.f };
            memcpy(tr_frame + tr_off[TR_PCOL], particle_color, 16);
            memcpy(tr_frame + tr_off[TR_BCOL], beam_color, 16);
            const float one = 1.f;
            uint32_t sprite_info[4] = { 1u, 0u, 0xffffffffu, 0u };      // texture 1, alpha 1, no tint, not a corona
            memcpy(&sprite_info[1], &one, 4);
            memcpy(tr_frame + tr_off[TR_SINFO], sprite_info, 16);
            const D3D12_RAYTRACING_AABB box = { beam_x - beam_r, beam_y0 - beam_r, fz - beam_r,
                                                beam_x + beam_r, beam_y1 + beam_r, fz + beam_r };
            memcpy(tr_frame + tr_off[TR_AABB], &box, sizeof(box));
            // pt_logic_beam_intersection's world_to_beam: the beam runs from
            // (beam_x, beam_y0, fz) up +y; in beam space from the origin up +z.
            // Its columns (1,0,0), (0,0,1), (0,-1,0) as halves, then the
            // translation -R*start as floats; the radius and length.
            const float t4[4] = { -beam_x, fz, -beam_y0, 1.f };
            const float radius = beam_r, length = beam_y1 - beam_y0;
            uint32_t bi[12];
            memcpy(bi, t4, 16);
            bi[4] = 0x3c00u; bi[5] = 0u; bi[6] = 0u; bi[7] = 0x3c00u;
            bi[8] = 0xbc000000u; bi[9] = 0u;
            memcpy(&bi[10], &radius, 4);
            memcpy(&bi[11], &length, 4);
            memcpy(tr_frame + tr_off[TR_BINT], bi, sizeof(bi));
        }
        ID3D12Resource *tr_staging = upload(tr_frame, tr_upload);
        ID3D12Resource *tr6[TR_STREAMS];
        for (int i = 0; i < TR_STREAMS; i++)
            tr6[i] = buffer(D3D12_HEAP_TYPE_DEFAULT, tr_capacity, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        // create_buffer_views', as d3d12_world_transparency_init writes them
        d3d12_set_write_texel_buffer(&rt_set, RT_BINDING_PARTICLE_COLOR, 0, tr6[TR_PCOL], DXGI_FORMAT_R32G32B32A32_FLOAT,
                                     0, (uint32_t)(tr_capacity / 16));
        d3d12_set_write_texel_buffer(&rt_set, RT_BINDING_BEAM_COLOR, 0, tr6[TR_BCOL], DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                     (uint32_t)(tr_capacity / 16));
        d3d12_set_write_texel_buffer(&rt_set, RT_BINDING_SPRITE_INFO, 0, tr6[TR_SINFO], DXGI_FORMAT_R32G32B32A32_UINT, 0,
                                     (uint32_t)(tr_capacity / 16));
        d3d12_set_write_texel_buffer(&rt_set, RT_BINDING_BEAM_INTERSECT, 0, tr6[TR_BINT], DXGI_FORMAT_R32G32B32A32_UINT,
                                     0, (uint32_t)(tr_capacity / 16));

        // two TLASes in one pool with one scratch, as world.c's frame slot
        d3d12_as_sizes_t ts6;
        if (!d3d12_tlas_sizes(dev, 4, bf, &ts6)) {
            printf("FAIL: no TLAS sizes\n");
            return 1;
        }
        d3d12_as_pool_t pool6;
        if (!d3d12_as_pool_create(&pool6, (ID3D12Device *)dev, 2 * FX_ALIGN(ts6.result) + D3D12_AS_ALIGNMENT,
                                  L"phase 6 TLASes")) {
            printf("FAIL: AS pool\n");
            return 1;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS tlas6 = d3d12_as_pool_alloc(&pool6, ts6.result);
        const D3D12_GPU_VIRTUAL_ADDRESS tlas6_fx = d3d12_as_pool_alloc(&pool6, ts6.result);
        ID3D12Resource *scratch6 = buffer(D3D12_HEAP_TYPE_DEFAULT, 2 * FX_ALIGN(ts6.scratch),
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, tlas6);
        d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_EFFECTS, tlas6_fx);

        // the plan: an empty run (no BLAS), the explosion's run, the particle's
        // quad, the beam's box, the sprite's quad from its vertex offset
        const D3D12_GPU_VIRTUAL_ADDRESS pos6_va = ID3D12Resource_GetGPUVirtualAddress(pos6);
        const D3D12_GPU_VIRTUAL_ADDRESS vert_va = ID3D12Resource_GetGPUVirtualAddress(tr6[TR_VERT]);
        const D3D12_GPU_VIRTUAL_ADDRESS idx_va = ID3D12Resource_GetGPUVirtualAddress(idx6);
        d3d12_dynamic_blas_t fx[5];
        memset(fx, 0, sizeof(fx));
        fx[0].geometry = d3d12_triangle_run(pos6_va, 0, 0);
        fx[1].geometry = d3d12_triangle_run(pos6_va, explo_offset, 4);
        fx[2].geometry.vertices = vert_va;
        fx[2].geometry.num_vertices = 4;
        fx[2].geometry.indices = idx_va;
        fx[2].geometry.num_indices = 6;
        fx[3].geometry.aabbs = ID3D12Resource_GetGPUVirtualAddress(tr6[TR_AABB]);
        fx[3].geometry.num_aabbs = 1;
        fx[4].geometry.vertices = vert_va + sprite_vertex_offset;
        fx[4].geometry.num_vertices = 4;
        fx[4].geometry.indices = idx_va;
        fx[4].geometry.num_indices = 6;
        d3d12_dynamic_as_t dyn6;
        memset(&dyn6, 0, sizeof(dyn6));
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS fx_flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        if (!d3d12_dynamic_blas_plan(dev, &dyn6, fx, 5, fx_flags) || fx[0].address || !fx[1].address ||
            !fx[2].address || !fx[3].address || !fx[4].address) {
            printf("FAIL: phase 6's BLAS plan (%llx %llx %llx %llx %llx: the first should be 0)\n",
                   (unsigned long long)fx[0].address, (unsigned long long)fx[1].address,
                   (unsigned long long)fx[2].address, (unsigned long long)fx[3].address,
                   (unsigned long long)fx[4].address);
            return 1;
        }

        // the instance list: the world, then the effects as vkpt_pt_append_dynamic_instances appends them
        ID3D12Resource *inst6_up = upload(NULL, 5 * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        {
            D3D12_RAYTRACING_INSTANCE_DESC *d = NULL;
            ID3D12Resource_Map(inst6_up, 0, NULL, (void **)&d);
            memset(d, 0, 5 * sizeof(*d));
            d[0] = inst;
            const struct { uint32_t id, sbto, flags; D3D12_GPU_VIRTUAL_ADDRESS as; } e[4] = {
                { explo_offset, SBTO_EXPLOSION, 0, fx[1].address },
                { 0, SBTO_PARTICLE, D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE, fx[2].address },
                { 0, SBTO_BEAM, D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE, fx[3].address },
                { 0, SBTO_SPRITE, D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE, fx[4].address },
            };
            for (int i = 0; i < 4; i++) {
                D3D12_RAYTRACING_INSTANCE_DESC *x = &d[1 + i];
                x->Transform[0][0] = x->Transform[1][1] = x->Transform[2][2] = 1.f;
                x->InstanceID = e[i].id;
                x->InstanceMask = AS_FLAG_EFFECTS;
                x->InstanceContributionToHitGroupIndex = e[i].sbto;
                x->Flags = e[i].flags;
                x->AccelerationStructure = e[i].as;
            }
            ID3D12Resource_Unmap(inst6_up, 0, NULL);
        }
        {
            void *mp = NULL;
            ID3D12Resource_Map(ubo_buf, 0, NULL, &mp);
            QVKUniformBuffer_t *u = (QVKUniformBuffer_t *)mp;
            lit_ubo(u, frame, false, 0.f, false, taa_w, taa_h);
            u->pt_reflect_refract = 0.f;
            u->flt_taa = AA_MODE_UPSCALE;
            // the effects' scales, 1 so their colours read plainly
            u->prev_adapted_luminance = 1.f;
            u->pt_particle_softness = 1.f;
            u->pt_particle_brightness = 1.f;
            u->pt_beam_softness = 1.f;
            u->pt_explosion_brightness = 1.f;
            ID3D12Resource_Unmap(ubo_buf, 0, NULL);
        }

        // world.c's order: the transparency copies, the per-frame BLASes, both TLASes, the passes
        ID3D12CommandAllocator_Reset(ca);
        ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
        ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
        for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
            if (i == frame % NUM_LIGHT_STATS_BUFFERS)
                d3d12_clear_buffer(cl, stats[i], stats_size, zeros);
        for (int i = 0; i < TR_STREAMS; i++) {
            ID3D12GraphicsCommandList4_CopyBufferRegion(cl, tr6[i], 0, tr_staging, tr_off[i], tr_size[i]);
            transition(cl, tr6[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        transition(cl, idx6, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cl, pos6, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cl, pos6, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d3d12_dynamic_blas_build(cl, &dyn6, fx, 5, fx_flags);
        const D3D12_GPU_VIRTUAL_ADDRESS inst6_va = ID3D12Resource_GetGPUVirtualAddress(inst6_up);
        const D3D12_GPU_VIRTUAL_ADDRESS scratch6_va = ID3D12Resource_GetGPUVirtualAddress(scratch6);
        d3d12_tlas_build(cl, inst6_va, 1, bf, tlas6, scratch6_va);
        d3d12_tlas_build(cl, inst6_va + sizeof(D3D12_RAYTRACING_INSTANCE_DESC), 4, bf, tlas6_fx,
                         scratch6_va + FX_ALIGN(ts6.scratch));
        d3d12_as_barrier(cl, &pool6);
        {
            const int odd = frame & 1;
            const d3d12_passes_t p = {
                .cmd = cl, .enhanced_barriers = enhanced, .psos = psos,
                .pt_sets = { &rt_set, &ubo_set, &textures[odd], &vb_set, &tiles_set },
                .standard_sets = { &ubo_set, &textures[odd], &vb_set },
                .tonemap_buffer = tonemap, .tonemap_size = sizeof(ToneMappingBuffer), .zero_buffer = zeros,
            };
            const d3d12_passes_frame_t pf = {
                .field_width = W / 2, .slice_width = W, .render_width = W, .render_height = H,
                .taa_output_width = W, .taa_output_height = H, .taa_image_width = taa_w, .taa_image_height = taa_h,
                .enable_denoiser = false, .reflect_refract = 0, .num_bounce_rays = 0.f,
                .tone_mapping = true, .tm_reset = true, .frame_time = 1.f / 60.f,
                .tm_slope_blur_sigma = 6.f, .tm_knee_start = 0.9f, .tm_white_point = 10.f,
            };
            d3d12_record_passes(&p, &pf);
        }
        if (!run(cl)) {
            printf("FAIL: phase 6's frame did not complete (device %s)\n",
                   SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
            return 1;
        }
        const int fx_odd = frame & 1;
        frame++;

        // PT_TRANSPARENT over the lower half: each effect's colour inside it,
        // nothing outside; a launch near an outline is not called either way
        if (!read_image(cl, ca, img[VKPT_IMG_PT_TRANSPARENT], W, H, pixels))
            return 1;
        enum { FX_NONE, FX_PARTICLE, FX_SPRITE, FX_BEAM, FX_EXPLOSION, FX_SKIP };
        static const char *fx_names[FX_SKIP] = { "nothing", "the particle (red)", "the sprite (blue)", "the beam (green)",
                                                 "the explosion (white)" };
        int fx_wrong = 0, fx_seen[FX_SKIP] = { 0 }, fx_edge = 0;
        for (int z = 0; z < 2; z++)
            for (int ly = H / 2; ly < H; ly++)
                for (int lx = 0; lx < W / 2; lx++) {
                    const int ix = lx + z * (W / 2);
                    const int sx = z == 0 ? lx * 2 + (ly & 1) : lx * 2 + 1 - (ly & 1);
                    const float dx = (((sx + 0.5f) / W) * 2.f - 1.f) * LIT_TAN_X;
                    const float dy = -(((ly + 0.5f) / H) * 2.f - 1.f) * LIT_TAN_Y;
                    const float hx = dx * fz, hy = dy * fz;
                    int expect = FX_NONE;
                    const float *quads[3] = { particle_r, sprite_r, explo_r };
                    static const int quad_fx[3] = { FX_PARTICLE, FX_SPRITE, FX_EXPLOSION };
                    for (int q = 0; q < 3; q++) {
                        const float *r = quads[q];
                        const float m = fminf(fminf(hx - r[0], r[1] - hx), fminf(hy - r[2], r[3] - hy));
                        if (m < -0.5f)
                            continue;                   // outside, clear of the outline
                        if (m < 0.5f) {
                            expect = FX_SKIP;           // on the outline
                            break;
                        }
                        expect = quad_fx[q];
                        if (q == 0) {
                            // the particle is a disc: factor 1 - 2 |uv - 0.5|
                            const float u = (hx - r[0]) / (r[1] - r[0]) - 0.5f, v = (hy - r[2]) / (r[3] - r[2]) - 0.5f;
                            const float d = sqrtf(u * u + v * v);
                            if (d > 0.3f && d < 0.55f)
                                expect = FX_SKIP;
                            else if (d >= 0.55f)
                                expect = FX_NONE;
                        }
                        break;
                    }
                    if (expect == FX_NONE) {
                        // the ray's closest approach to the beam's axis (x = beam_x, z = fz)
                        const float d = fabsf(fz * dx - beam_x) / sqrtf(1.f + dx * dx);
                        const bool along = hy > beam_y0 + 1.f && hy < beam_y1 - 1.f;
                        const bool near_box = hy > beam_y0 - beam_r - 1.f && hy < beam_y1 + beam_r + 1.f &&
                                              d < beam_r + 0.5f;
                        if (along && d < 0.5f * beam_r)
                            expect = FX_BEAM;
                        else if (near_box)
                            expect = FX_SKIP;
                    }
                    if (expect == FX_SKIP) {
                        fx_edge++;
                        continue;
                    }
                    const float *px = pixels + ((size_t)ly * W + ix) * 4;
                    const float big = fmaxf(fmaxf(px[0], px[1]), px[2]);
                    bool ok;
                    switch (expect) {
                    case FX_PARTICLE:
                        ok = px[3] > 0.01f && px[0] > 0.f && px[1] <= 1e-3f * px[0] && px[2] <= 1e-3f * px[0];
                        break;
                    case FX_SPRITE:
                        ok = px[3] > 0.01f && px[2] > 0.f && px[0] <= 1e-3f * px[2] && px[1] <= 1e-3f * px[2];
                        break;
                    case FX_BEAM:
                        ok = px[3] > 0.01f && px[1] > 0.f && px[0] <= 1e-3f * px[1] && px[2] <= 1e-3f * px[1];
                        break;
                    case FX_EXPLOSION:
                        ok = px[3] > 0.01f && px[0] > 0.f && fabsf(px[1] - px[0]) <= 0.01f * big &&
                             fabsf(px[2] - px[0]) <= 0.01f * big;
                        break;
                    default:
                        ok = fabsf(px[3]) < 1e-6f && big < 1e-6f;
                        break;
                    }
                    if (ok)
                        fx_seen[expect]++;
                    else if (fx_wrong++ < 8)
                        printf("    pixel (%d,%d) field %d: got (%.4g %.4g %.4g %.4g), expected %s\n", sx, ly, z, px[0],
                               px[1], px[2], px[3], fx_names[expect]);
                }
        int nonfinite = 0;
        if (!read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_TAA_OUTPUT, fx_odd)], W, H, pixels))
            return 1;
        for (size_t i = 0; i < (size_t)W * H * 4; i++)
            nonfinite += !isfinite(pixels[i]);
        const bool fx_ok = fx_wrong == 0 && fx_seen[FX_PARTICLE] && fx_seen[FX_SPRITE] && fx_seen[FX_BEAM] &&
                           fx_seen[FX_EXPLOSION] && fx_seen[FX_NONE] && nonfinite == 0;
        printf("  PT_TRANSPARENT: %d launches wrong; right: %d particle, %d sprite, %d beam, %d explosion, %d clear; "
               "%d too near an outline to call; %d non-finite%s\n", fx_wrong, fx_seen[FX_PARTICLE], fx_seen[FX_SPRITE],
               fx_seen[FX_BEAM], fx_seen[FX_EXPLOSION], fx_seen[FX_NONE], fx_edge, nonfinite, fx_ok ? "" : "  <-- FAIL");

        d3d12_dynamic_as_destroy(&dyn6);
        d3d12_as_pool_destroy(&pool6);
        ID3D12Resource_Release(scratch6);
        ID3D12Resource_Release(inst6_up);
        ID3D12Resource_Release(tr_staging);
        for (int i = 0; i < TR_STREAMS; i++)
            ID3D12Resource_Release(tr6[i]);
        ID3D12Resource_Release(idx6);
        const int phase_errors = errors - errors_before;
        printf("  %d validation errors in phase 6\n", phase_errors);
        if (!fx_ok || phase_errors)
            fx_failed = 1;
    }

    /* --------------------------------------------------------------------- */
    /* PHASE 7: blood                                                         */
    /* --------------------------------------------------------------------- */
    /* world.c's upload_blood, through passes.c's d3d12_record_blood_upload,
       as vkpt_blood_prepare's frames drive it: instanced buffers holding
       somebody else's bytes, then
         0. an epoch frame - the section zeroed between blood.c's regions (one
            gap well past D3D12_ZERO_BUFFER_SIZE in both buffers, so the
            zeroing runs in chunks), the regions copied out of the staging, and
            one region running past the section's end skipped (and zeroed)
         1. an ordinary frame, which writes its one region and nothing else
         2. a frame with nothing to send, which records nothing
       Every byte of both buffers is read back after each and checked against
       the same frame applied on the CPU. */
    int blood_failed = 0;
    {
        printf("\nphase 7: blood (passes.c's d3d12_record_blood_upload)\n");
        const int errors_before = errors;
        enum { BN = 64000, BFIRST = 2000, BCOUNT = 60000 };     // the buffers, the section in them
        const uint32_t bsize[2] = { (uint32_t)sizeof(VboPrimitive), 9 * (uint32_t)sizeof(float) };
        ID3D12Resource *bdev[2], *bstage[2];
        uint8_t *bmap[2], *bexpect[2], *bgot[2];
        for (int k = 0; k < 2; k++) {
            const uint64_t dev_size = (uint64_t)BN * bsize[k];
            bdev[k] = buffer(D3D12_HEAP_TYPE_DEFAULT, dev_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
            bstage[k] = buffer(D3D12_HEAP_TYPE_UPLOAD, (uint64_t)BCOUNT * bsize[k], D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
            ID3D12Resource_Map(bstage[k], 0, NULL, (void **)&bmap[k]);
            bexpect[k] = malloc(dev_size);
            bgot[k] = malloc(dev_size);
            memset(bexpect[k], 0xa5, dev_size);     // somebody else's bytes
        }
        {
            ID3D12Resource *fill[2] = { upload(bexpect[0], (uint64_t)BN * bsize[0]),
                                        upload(bexpect[1], (uint64_t)BN * bsize[1]) };
            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            for (int k = 0; k < 2; k++)
                ID3D12GraphicsCommandList4_CopyBufferRegion(cl, bdev[k], 0, fill[k], 0, (uint64_t)BN * bsize[k]);
            const bool ok = run(cl);
            for (int k = 0; k < 2; k++)
                ID3D12Resource_Release(fill[k]);
            if (!ok) {
                printf("FAIL: phase 7's buffers\n");
                return 1;
            }
        }

        // blood.c's regions: src in its shadow copy (and the staging), dst in the instanced buffers
        static const d3d12_blood_region_t epoch_regions[] = {
            { 0, BFIRST + 0, 100 },             // at the section's start: no gap before it
            { 500, BFIRST + 500, 50 },
            { 45000, BFIRST + 45000, 1000 },    // after a gap of 44450 primitives
            { 59990, BFIRST + 59990, 20 },      // runs 10 past the section's end: skipped, zeroed
        };
        static const d3d12_blood_region_t later_regions[] = { { 600, BFIRST + 600, 10 } };
        const struct {
            const d3d12_blood_region_t *regions;
            uint32_t num_regions;
            bool zero_section, records;
        } bframes[3] = {
            { epoch_regions, 4, true, true },
            { later_regions, 1, false, true },
            { NULL, 0, false, false },
        };
        int bwrong = 0, bfirst_wrong = -1;
        for (int f = 0; f < 3 && bwrong == 0; f++) {
            // this frame's staging: never zero, and different each frame
            for (int k = 0; k < 2; k++)
                for (uint64_t o = 0; o < (uint64_t)BCOUNT * bsize[k]; o++)
                    bmap[k][o] = (uint8_t)((((uint32_t)o * 2654435761u) >> 13) + (uint32_t)f * 77u + (uint32_t)k * 31u) | 1;

            const d3d12_blood_upload_t up = {
                .prims = bdev[0], .positions = bdev[1],
                .staging_prims = bstage[0], .staging_positions = bstage[1], .staging_capacity = BCOUNT,
                .prim_size = bsize[0], .position_size = bsize[1],
                .zeros = zeros,
                .first = BFIRST, .end = BFIRST + BCOUNT,
                .zero_section = bframes[f].zero_section,
                .regions = bframes[f].regions, .num_regions = bframes[f].num_regions,
            };
            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            const bool recorded = d3d12_record_blood_upload(cl, &up);
            if (!run(cl)) {
                printf("FAIL: phase 7 frame %d did not complete\n", f);
                return 1;
            }
            if (recorded != bframes[f].records) {
                printf("    frame %d: d3d12_record_blood_upload said %d, expected %d\n", f, recorded, bframes[f].records);
                bwrong++;
            }

            // the frame on the CPU: the section zeroed on an epoch, then each region in range
            for (int k = 0; k < 2; k++) {
                if (bframes[f].zero_section)
                    memset(bexpect[k] + (size_t)BFIRST * bsize[k], 0, (size_t)BCOUNT * bsize[k]);
                for (uint32_t r = 0; r < bframes[f].num_regions; r++) {
                    const d3d12_blood_region_t *g = &bframes[f].regions[r];
                    if (g->dst < BFIRST || g->dst + g->count > BFIRST + BCOUNT || g->src + g->count > BCOUNT)
                        continue;
                    memcpy(bexpect[k] + (size_t)g->dst * bsize[k], bmap[k] + (size_t)g->src * bsize[k],
                           (size_t)g->count * bsize[k]);
                }
                if (!read_buffer(cl, ca, bdev[k], 0, (uint64_t)BN * bsize[k], bgot[k]))
                    return 1;
                for (uint32_t i = 0; i < BN; i++) {
                    if (memcmp(bgot[k] + (size_t)i * bsize[k], bexpect[k] + (size_t)i * bsize[k], bsize[k])) {
                        if (bwrong < 8)
                            printf("    frame %d, %s, primitive %u: got %02x.. expected %02x..\n", f,
                                   k ? "positions" : "primitives", i, bgot[k][(size_t)i * bsize[k]],
                                   bexpect[k][(size_t)i * bsize[k]]);
                        if (bfirst_wrong < 0)
                            bfirst_wrong = f;
                        bwrong++;
                    }
                }
            }
        }
        printf("  the instanced buffers after an epoch frame, an ordinary one and an empty one: %d primitives wrong%s\n",
               bwrong, bwrong ? "  <-- FAIL" : "");

        for (int k = 0; k < 2; k++) {
            ID3D12Resource_Unmap(bstage[k], 0, NULL);
            ID3D12Resource_Release(bstage[k]);
            ID3D12Resource_Release(bdev[k]);
            free(bexpect[k]);
            free(bgot[k]);
        }
        const int phase_errors = errors - errors_before;
        printf("  %d validation errors in phase 7\n", phase_errors);
        if (bwrong || phase_errors)
            blood_failed = 1;
    }

    /* --------------------------------------------------------------------- */
    /* PHASE 8: the fog                                                       */
    /* --------------------------------------------------------------------- */
    int fog_failed = 0;
    {
        const int errors_before = errors;
        printf("\nphase 8: the fog (shadow_map.c's draws, god_rays.c's march, froxel grid and filter)\n");

        // phase 1's world and TLAS again (phase 6 bound its own, since destroyed),
        // and its one instance
        d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_GEOMETRY, tlas);
        d3d12_set_write_as(&rt_set, RT_BINDING_TLAS, TLAS_INDEX_EFFECTS, 0);
        d3d12_set_write_buffer(&vb_set, PRIMITIVE_BUFFER_BINDING_IDX, VERTEX_BUFFER_WORLD, world, 0, primitives_size);
        InstanceBuffer *ib8 = calloc(1, sizeof(InstanceBuffer));
        ib8->tlas_instance_model_indices[0] = -1;
        ib8->tlas_instance_prim_offsets[0] = 0;
        ID3D12Resource *ib8_up = upload(ib8, sizeof(InstanceBuffer));
        free(ib8);

        // the sun's shadow map, as world.c's fog_init: two D32 layers (opaque, water),
        // a DSV each; smaller than SHADOWMAP_SIZE - the shaders sample it normalised
        enum { SMAP = 1024 };
        ID3D12Resource *smap = NULL;
        ID3D12DescriptorHeap *dsv_heap = NULL;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv[2];
        {
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd = { 0 };
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = SMAP; rd.Height = SMAP; rd.DepthOrArraySize = 2; rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R32_TYPELESS; rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_D32_FLOAT };
            cv.DepthStencil.Depth = 1.f;
            D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2 };
            if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource,
                                                             (void **)&smap)) ||
                FAILED(ID3D12Device5_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap))) {
                printf("FAIL: phase 8's shadow map\n");
                return 1;
            }
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv[0]);
            dsv[1] = dsv[0];
            dsv[1].ptr += ID3D12Device5_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            for (int i = 0; i < 2; i++) {
                D3D12_DEPTH_STENCIL_VIEW_DESC dv = { 0 };
                dv.Format = DXGI_FORMAT_D32_FLOAT;
                dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dv.Texture2DArray.FirstArraySlice = (UINT)i;
                dv.Texture2DArray.ArraySize = 1;
                ID3D12Device5_CreateDepthStencilView(dev, smap, &dv, dsv[i]);
            }
        }
        ID3D12PipelineState *smap_pso8 = NULL;
        {
            snprintf(path, sizeof(path), "%sbaseq2\\shader_d3d12\\shadow_map.vert.dxil", root);
            size_t vs_size = 0;
            void *vs = load(path, &vs_size);
            if (!vs) { printf("FAIL: no %s\n", path); return 1; }
            D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
            D3D12_INPUT_ELEMENT_DESC element;
            d3d12_sky_shadowmap_pso_desc(&gd, &element, d3d12_pipeline_layouts[D3D12_PL_SHADOW_MAP].root_signature,
                                         vs, vs_size);
            if (FAILED(ID3D12Device5_CreateGraphicsPipelineState(dev, &gd, &IID_ID3D12PipelineState,
                                                                 (void **)&smap_pso8)))
                smap_pso8 = NULL;
            free(vs);
            if (!smap_pso8) { printf("FAIL: the shadow map's pipeline\n"); return 1; }
        }

        // the froxel volumes, as d3d12_world_froxel_create (committed memory starts
        // zeroed): scatter, integrated, reservoirs, sky by frame slot, sky exposure
        enum { V_SCATTER = 0, V_INTEGRATED = 2, V_RESERVOIR = 4, V_SKY = 6, V_EXPOSURE = 8, NUM_V = 9 };
        ID3D12Resource *vol[NUM_V];
        DXGI_FORMAT vfmt[NUM_V];
        for (int v = 0; v < NUM_V; v++) {
            vfmt[v] = v == V_EXPOSURE ? DXGI_FORMAT_R32_UINT :
                      (v == V_RESERVOIR || v == V_RESERVOIR + 1) ? DXGI_FORMAT_R32G32B32A32_FLOAT :
                                                                    DXGI_FORMAT_R16G16B16A16_FLOAT;
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd = { 0 };
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            rd.Width = v == V_EXPOSURE ? SKY_EXPOSURE_DIM_X : FROXEL_GRID_X;
            rd.Height = v == V_EXPOSURE ? SKY_EXPOSURE_DIM_Y : FROXEL_GRID_Y;
            rd.DepthOrArraySize = v == V_EXPOSURE ? SKY_EXPOSURE_DIM_Z : FROXEL_GRID_Z;
            rd.MipLevels = 1;
            rd.Format = vfmt[v];
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
            if (FAILED(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                             D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource,
                                                             (void **)&vol[v]))) {
                printf("FAIL: froxel volume %d\n", v);
                return 1;
            }
        }
        ID3D12Resource *mask = buffer(D3D12_HEAP_TYPE_DEFAULT, SKY_EXPOSURE_MASK_WORDS * sizeof(uint32_t),
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        // the god rays sets, as world.c's write_god_rays_set: set i writes slot i's
        // scatter and sky volumes and samples the other slot's as their history
        d3d12_set_t gr_set[2];
        for (int i = 0; i < 2; i++) {
            const int o = 1 - i;
            if (!d3d12_set_alloc(&gr_set[i], D3D12_SET_GOD_RAYS, "god rays"))
                return 1;
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
            d3d12_set_write_srv_image(&gr_set[i], 0, 0, smap, &sv, &sd);
            d3d12_set_write_as(&gr_set[i], 1, 0, tlas);

            const struct { unsigned binding; int v; bool sampled; } vb8[9] = {
                { 2, V_SCATTER + i, false }, { 3, V_SCATTER + o, true }, { 4, V_INTEGRATED + i, false },
                { 5, V_INTEGRATED + i, true }, { 6, V_RESERVOIR, false }, { 7, V_RESERVOIR + 1, false },
                { 8, V_SKY + i, false }, { 9, V_SKY + o, true }, { 10, V_EXPOSURE, false },
            };
            for (int k = 0; k < 9; k++) {
                const int v = vb8[k].v;
                if (vb8[k].sampled) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC vs = { 0 };
                    vs.Format = vfmt[v];
                    vs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    vs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    vs.Texture3D.MipLevels = 1;
                    D3D12_SAMPLER_DESC fs = { 0 };
                    fs.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                    fs.AddressU = fs.AddressV = fs.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                    fs.MaxAnisotropy = 1;
                    fs.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                    fs.MaxLOD = D3D12_FLOAT32_MAX;
                    d3d12_set_write_srv_image(&gr_set[i], vb8[k].binding, 0, vol[v], &vs, &fs);
                } else {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = { 0 };
                    uv.Format = vfmt[v];
                    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                    uv.Texture3D.WSize = v == V_EXPOSURE ? SKY_EXPOSURE_DIM_Z : FROXEL_GRID_Z;
                    d3d12_set_write_uav_image(&gr_set[i], vb8[k].binding, 0, vol[v], &uv);
                }
            }
            d3d12_set_write_buffer(&gr_set[i], 11, 0, mask, 0, SKY_EXPOSURE_MASK_WORDS * sizeof(uint32_t));
        }

        // the sun's view-projection: up and ahead of the camera (fog_ubo says why)
        const float sun_len = sqrtf(0.6f * 0.6f + 1.f);
        const float sun_dir[3] = { 0.f, 0.6f / sun_len, 1.f / sun_len };
        float VP[16], depth_scale;
        fog_shadow_viewproj(sun_dir, fog_box_min, fog_box_max, VP, &depth_scale);
        // quad B moved 50 units back by its own draw's matrix (an instance's model matrix)
        float VPm[16];
        memcpy(VPm, VP, sizeof(VPm));
        for (int r = 0; r < 4; r++)
            VPm[12 + r] += VP[8 + r] * 50.f;

        // vkpt_shadow_map_render as world.c's two halves draw it: quad A (primitives
        // 0-3) and quad B (4-7, its own view of the buffer, moved) into the opaque
        // layer; quad B again, by its first vertex, into the water layer. The world
        // buffer is COMMON -> VERTEX_AND_CONSTANT_BUFFER -> COMMON around them.
        {
            const D3D12_GPU_VIRTUAL_ADDRESS pos = ID3D12Resource_GetGPUVirtualAddress(world) + primitives_size;
            d3d12_shadow_draw_t draws[2], water;
            memset(draws, 0, sizeof(draws));
            draws[0].vertices.BufferLocation = pos;
            draws[0].vertices.SizeInBytes = (UINT)num_prims * 36;
            draws[0].vertices.StrideInBytes = 12;
            draws[0].first = 0;
            draws[0].count = 12;
            memcpy(draws[0].mvp, VP, sizeof(VP));
            draws[1].vertices.BufferLocation = pos + 4 * 36;
            draws[1].vertices.SizeInBytes = 4 * 36;
            draws[1].vertices.StrideInBytes = 12;
            draws[1].first = 0;
            draws[1].count = 12;
            memcpy(draws[1].mvp, VPm, sizeof(VPm));
            water = draws[0];
            water.first = 12;
            water.count = 12;

            ID3D12CommandAllocator_Reset(ca);
            ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
            ID3D12GraphicsCommandList4_CopyBufferRegion(cl, instance_buffer, 0, ib8_up, 0, sizeof(InstanceBuffer));
            transition(cl, world, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            d3d12_record_shadow_map(cl, smap_pso8, dsv[0], SMAP, true, draws, 2);
            d3d12_record_shadow_map(cl, smap_pso8, dsv[1], SMAP, true, &water, 1);
            transition(cl, world, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COMMON);
            transition(cl, smap, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (!run(cl)) {
                printf("FAIL: phase 8's shadow map draws did not complete\n");
                return 1;
            }
        }

        // the depths: quad A's centre, moved quad B's centre, empty air (layer 0);
        // quad B's centre and quad A's (nothing there) in the water layer
        int smap_wrong = 0;
        {
            float *depth = malloc((size_t)SMAP * SMAP * sizeof(float));
            const struct { int layer; const float *m; float p[3]; bool covered; const char *name; } probes[] = {
                { 0, VP,  { -500.f, -500.f, 100.f }, true,  "quad A" },
                { 0, VPm, {  500.f, -500.f, 200.f }, true,  "quad B, moved by its matrix" },
                { 0, VP,  {    0.f,  900.f, 300.f }, false, "air above the quads" },
                { 1, VP,  {  500.f, -500.f, 200.f }, true,  "quad B as water" },
                { 1, VP,  { -500.f, -500.f, 100.f }, false, "quad A, not water" },
            };
            int read_layer = -1;
            for (size_t k = 0; k < sizeof(probes) / sizeof(probes[0]); k++) {
                if (probes[k].layer != read_layer) {
                    if (!read_depth_layer(cl, ca, smap, (UINT)probes[k].layer, SMAP, depth))
                        return 1;
                    read_layer = probes[k].layer;
                }
                int tx, ty;
                float expect;
                fog_shadow_texel(probes[k].m, probes[k].p, SMAP, &tx, &ty, &expect);
                if (!probes[k].covered)
                    expect = 1.f;
                const bool inside = tx >= 0 && ty >= 0 && tx < SMAP && ty < SMAP;
                const float got = inside ? depth[(size_t)ty * SMAP + tx] : NAN;
                const bool ok = inside && fabsf(got - expect) <= 0.01f;
                printf("  shadow map layer %d, %-28s texel (%4d,%4d): depth %.4f, expected %.4f%s\n", probes[k].layer,
                       probes[k].name, tx, ty, got, expect, ok ? "" : "  <-- FAIL");
                smap_wrong += !ok;
            }
            free(depth);
        }

        // the frames: the lit frame with plain compositing, no fog, then the classic
        // god rays, then map fog on the grid (a bake batch on its first frame, the
        // bake `ready` from its fourth, as vkpt says so MAX_FRAMES_IN_FLIGHT later)
        static const struct { const char *name; int mode; int frames; } fog_configs[] = {
            { "no fog",                                      0, 4 },
            { "classic god rays (march, filter)",            1, 6 },
            { "map fog on the froxel grid, ReSTIR, the bake", 2, 8 },
        };
        float *transparent = calloc((size_t)W * H * 4, sizeof(float));
        double miss_fog[3] = { 0, 0, 0 };
        int pic_bad = 0;
        uint32_t taa_w8, taa_h8;
        d3d12_image_extent(VKPT_IMG_TAA_OUTPUT, &ext, &taa_w8, &taa_h8);
        for (int c = 0; c < 3; c++) {
            const int mode = fog_configs[c].mode;
            for (int k = 0; k < fog_configs[c].frames; k++, frame++) {
                const int odd = frame & 1;
                {
                    void *pm = NULL;
                    ID3D12Resource_Map(ubo_buf, 0, NULL, &pm);
                    QVKUniformBuffer_t *u = pm;
                    lit_ubo(u, frame, false, 1.f, false, taa_w8, taa_h8);
                    fog_ubo(u, mode, sun_dir, VP, depth_scale, mode == 2 && k >= 3);
                    ID3D12Resource_Unmap(ubo_buf, 0, NULL);
                }
                ID3D12CommandAllocator_Reset(ca);
                ID3D12GraphicsCommandList4_Reset(cl, ca, NULL);
                ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 2, heaps);
                for (int i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
                    if (i == frame % NUM_LIGHT_STATS_BUFFERS)
                        d3d12_clear_buffer(cl, stats[i], stats_size, zeros);
                const d3d12_passes_t p8 = {
                    .cmd = cl,
                    .enhanced_barriers = enhanced,
                    .psos = psos,
                    .pt_sets = { &rt_set, &ubo_set, &textures[odd], &vb_set, &tiles_set },
                    .standard_sets = { &ubo_set, &textures[odd], &vb_set },
                    .god_rays_sets = { &gr_set[odd], &vb_set, &ubo_set, &textures[odd] },
                    .tonemap_buffer = tonemap,
                    .tonemap_size = sizeof(ToneMappingBuffer),
                    .zero_buffer = zeros,
                    .heaps = heaps,
                    .num_heaps = 2,
                };
                const d3d12_passes_frame_t pf8 = {
                    .field_width = W / 2,
                    .slice_width = W,
                    .render_width = W,
                    .render_height = H,
                    .taa_output_width = W,
                    .taa_output_height = H,
                    .taa_image_width = taa_w8,
                    .taa_image_height = taa_h8,
                    .enable_denoiser = false,
                    .reflect_refract = 1,
                    .num_bounce_rays = 1.f,
                    .tone_mapping = true,
                    .tm_reset = k == 0,
                    .frame_time = 1.f / 60.f,
                    .tm_slope_blur_sigma = 6.f,
                    .tm_knee_start = 0.9f,
                    .tm_white_point = 10.f,
                    // R_RenderFrame_RTX's switches (vkpt_frame_prep_t's)
                    .god_rays = mode != 0,
                    .god_rays_march = true,
                    .froxel = mode == 2,
                    .froxel_restir = mode == 2,
                    // vkpt_sky_exposure_next_batch's first batch: the whole test grid
                    .sky_exposure_first = 0,
                    .sky_exposure_count = mode == 2 && k == 0 ? FOG_EXPOSURE_X * FOG_EXPOSURE_Y * FOG_EXPOSURE_Z : 0,
                };
                d3d12_record_passes(&p8, &pf8);
                if (!run(cl)) {
                    printf("FAIL: phase 8 frame %d (%s) did not complete (device %s)\n", frame, fog_configs[c].name,
                           SUCCEEDED(ID3D12Device5_GetDeviceRemovedReason(dev)) ? "fine" : "REMOVED");
                    return 1;
                }
            }

            // PT_TRANSPARENT, where the filter composites the fog: finite, and its
            // light over the misses (the upper half's rows); the picture finite
            const int last_odd = (frame - 1) & 1;
            if (!read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_PT_TRANSPARENT, last_odd)], W, H, transparent) ||
                !read_image(cl, ca, img[d3d12_image_at_binding(VKPT_IMG_TAA_OUTPUT, last_odd)], W, H, pixels))
                return 1;
            int nonfinite = 0;
            double miss = 0, hit = 0, alpha[2] = { 0, 0 }, peak = 0, taa[2] = { 0, 0 };
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    const float *t = transparent + ((size_t)y * W + x) * 4;
                    const float *px = pixels + ((size_t)y * W + x) * 4;
                    for (int ch = 0; ch < 4; ch++)
                        nonfinite += !isfinite(t[ch]) + !isfinite(px[ch]);
                    const double l = (t[0] + t[1] + t[2]) / 3.0;
                    peak = fmax(peak, l);
                    if (y < H / 2)
                        miss += l;
                    else
                        hit += l;
                    alpha[y >= H / 2] += t[3];
                    taa[y >= H / 2] += (px[0] + px[1] + px[2]) / 3.0;
                }
            const double half = W * H / 2;
            miss_fog[c] = miss / half;
            printf("  %-46s PT_TRANSPARENT rgb misses %.5f quads %.5f (peak %.5f), alpha %.4f %.4f; TAA %.4f %.4f; "
                   "%d non-finite%s\n", fog_configs[c].name, miss_fog[c], hit / half, peak, alpha[0] / half,
                   alpha[1] / half, taa[0] / half, taa[1] / half, nonfinite, nonfinite ? "  <-- FAIL" : "");
            pic_bad += nonfinite != 0;
        }
        // the god rays add light where no fog added none
        const bool lit = miss_fog[1] > miss_fog[0] + 1e-3 && miss_fog[2] > miss_fog[0] + 1e-3;
        printf("  the fog lights the misses (god rays %.5f, map fog %.5f, over no fog's %.5f)%s\n", miss_fog[1],
               miss_fog[2], miss_fog[0], lit ? "" : "  <-- FAIL");

        // the grid: the last frame's integrated volume finite and not empty; the
        // bake wrote the voxels it was given and nothing else
        int vbad = 0;
        {
            const int last_odd = (frame - 1) & 1;
            int nf, nz;
            double peak;
            if (!volume_stats(cl, ca, vol[V_INTEGRATED + last_odd], &nf, &nz, &peak))
                return 1;
            const bool ok = nf == 0 && nz > 0;
            printf("  integrated volume: %d of %d cells non-zero, peak %.4g, %d non-finite%s\n", nz,
                   FROXEL_GRID_X * FROXEL_GRID_Y * FROXEL_GRID_Z, peak, nf, ok ? "" : "  <-- FAIL");
            vbad += !ok;
            if (!volume_stats(cl, ca, vol[V_SCATTER + last_odd], &nf, &nz, &peak))
                return 1;
            printf("  scatter volume: %d cells non-zero, peak %.4g, %d non-finite%s\n", nz, peak, nf,
                   nf ? "  <-- FAIL" : "");
            vbad += nf != 0;
            if (!volume_stats(cl, ca, vol[V_EXPOSURE], &nf, &nz, &peak))
                return 1;
            const int baked = FOG_EXPOSURE_X * FOG_EXPOSURE_Y * FOG_EXPOSURE_Z;
            const bool bake_ok = nz > 0 && nz <= baked;
            printf("  sky exposure: %d of the %d baked voxels non-zero (none outside), largest %.0f%s\n", nz, baked,
                   peak, bake_ok ? "" : "  <-- FAIL");
            vbad += !bake_ok;
        }

        free(transparent);
        for (int v = 0; v < NUM_V; v++)
            ID3D12Resource_Release(vol[v]);
        ID3D12Resource_Release(mask);
        ID3D12Resource_Release(smap);
        ID3D12DescriptorHeap_Release(dsv_heap);
        ID3D12PipelineState_Release(smap_pso8);
        ID3D12Resource_Release(ib8_up);
        const int phase_errors = errors - errors_before;
        printf("  %d validation errors in phase 8\n", phase_errors);
        if (smap_wrong || pic_bad || !lit || vbad || phase_errors)
            fog_failed = 1;
    }

    printf("\n%d validation errors in all (phase 1: %d)\n", errors, phase1_errors);
    if (count_system_1358 && gbv)
        printf("(and %d of the system runtime's GPU-based validation 1358, not counted - see msg)\n", system_1358);
    d3d12_bindings_destroy_root_signatures();
    return (bad == 0 && blit_bad == 0 && lit_failed == 0 && sky_failed == 0 && refl_failed == 0 && dyn_failed == 0 && fx_failed == 0 &&
            blood_failed == 0 && fog_failed == 0 &&
            errors == 0) ? 0 : 1;
}
