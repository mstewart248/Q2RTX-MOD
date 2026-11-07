/*
What the D3D12 renderer borrows from vkpt - the only file in this directory
that includes vkpt.h.

vkpt is two things in one directory: a Vulkan backend, and an API-free
"frontend" that turns the game's data into what the shaders read - the 2D
queue, model loading, materials, (next) the BSP mesh, light lists, the global
UBO. The second half is reused, not copied: every fix to it reaches both
renderers, and the A/B between them compares backends, not two drifting
copies of the same code. What may be called from here must not touch Vulkan
state - qvk.device and everything made from it are NULL under vid_rtx 3.

Borrowed now:
  * the 2D API (R_Draw*_RTX, R_Set*_RTX) and its queue (vkpt/draw.c)
  * model loaders (MOD_Load*_RTX) and the CPU mesh trace (vkpt/models.c)
  * materials (vkpt/material.c), minus the `mat` command
  * the sampler choice per image (vkpt_image_sampler_kind, vkpt/textures.c)
  * map registration (vkpt_begin_registration): the BSP and its mesh, the
    light lists, the sky's per-map settings - and through d3d12_backend's
    load_world, the world buffer and static BLASes made here instead
  * the frame's CPU half (vkpt_prepare_frame): entities, lights, the UBO, the
    TLAS instance list - its staging writes land in world.c's upload buffers
  * the physical sky's CPU half (vkpt/physical_sky.c, precomputed_sky.c): the
    sun, the sky UBO fields, when the sky needs rendering, the preset's
    atmosphere, the terrain grid and shadow map view, R_SetSky_RTX's skybox -
    world.c and sky.c make the GPU side
*/

#include "../vkpt/vkpt.h"
#include "../vkpt/DLSS.h"
#include "../vkpt/DLSSNR.h"
#include "../vkpt/fg_present.h"
#include "../vkpt/reflex.h"
#include "../vkpt/material.h"
#include "../vkpt/physical_sky.h"
#include "../vkpt/precomputed_sky.h"
#include "d3d12pt.h"
#include "ngx.h"
#include "common/cpuprof.h"     // Sys_Microseconds (vid_present_stats)

// bindings.c sizes primitive_buffers[] without the engine headers
typedef char d3d12_max_models_agree[(D3D12_MAX_MODELS == MAX_MODELS) ? 1 : -1];
// world.c builds BLASes from 9 floats per primitive
typedef char d3d12_prim_positions_are_9_floats[(sizeof(prim_positions_t) == 9 * sizeof(float)) ? 1 : -1];

// the model sections world.c builds BLASes for are vkpt_dyn_blas_t's first five
typedef char d3d12_dyn_sections_agree[((int)D3D12_DYN_OPAQUE == (int)VKPT_DYN_OPAQUE &&
                                       (int)D3D12_DYN_TRANSPARENT == (int)VKPT_DYN_TRANSPARENT &&
                                       (int)D3D12_DYN_MASKED == (int)VKPT_DYN_MASKED &&
                                       (int)D3D12_DYN_VIEWER_WEAPON == (int)VKPT_DYN_VIEWER_WEAPON &&
                                       (int)D3D12_DYN_VIEWER_MODELS == (int)VKPT_DYN_VIEWER_MODELS &&
                                       (int)D3D12_DYN_BLOOD == (int)VKPT_DYN_BLOOD &&
                                       (int)D3D12_DYN_EXPLOSIONS == (int)VKPT_DYN_EXPLOSIONS &&
                                       (int)D3D12_DYN_PARTICLES == (int)VKPT_DYN_PARTICLES &&
                                       (int)D3D12_DYN_BEAMS == (int)VKPT_DYN_BEAMS &&
                                       (int)D3D12_DYN_SPRITES == (int)VKPT_DYN_SPRITES &&
                                       (int)D3D12_NUM_DYN == (int)VKPT_NUM_DYN) ? 1 : -1];
// blood.c's regions, as world.c reads them
typedef char d3d12_blood_regions_agree[(sizeof(d3d12_blood_region_t) == sizeof(vkpt_blood_region_t) &&
                                        offsetof(d3d12_blood_region_t, src) == offsetof(vkpt_blood_region_t, src) &&
                                        offsetof(d3d12_blood_region_t, dst) == offsetof(vkpt_blood_region_t, dst) &&
                                        offsetof(d3d12_blood_region_t, count) == offsetof(vkpt_blood_region_t, count))
                                       ? 1 : -1];
// transparency.c's streams, as world.c names them
typedef char d3d12_tr_streams_agree[((int)D3D12_TR_VERTICES == (int)VKPT_TR_VERTICES &&
                                     (int)D3D12_TR_BEAM_AABBS == (int)VKPT_TR_BEAM_AABBS &&
                                     (int)D3D12_TR_PARTICLE_COLORS == (int)VKPT_TR_PARTICLE_COLORS &&
                                     (int)D3D12_TR_BEAM_COLORS == (int)VKPT_TR_BEAM_COLORS &&
                                     (int)D3D12_TR_SPRITE_INFOS == (int)VKPT_TR_SPRITE_INFOS &&
                                     (int)D3D12_TR_BEAM_INTERSECT == (int)VKPT_TR_BEAM_INTERSECT &&
                                     (int)D3D12_TR_NUM_STREAMS == (int)VKPT_TR_NUM_STREAMS &&
                                     (int)D3D12_TR_INDICES == (int)VKPT_TR_INDICES) ? 1 : -1];

extern cvar_t *cvar_pt_caustics;    // vkpt/main.c, made by vkpt_init_frontend
extern SDL_Window *sdl_window;      // the client's window (video/sdl.c)
extern cvar_t *cvar_pt_blas_fast_trace, *cvar_pt_tlas_fast_trace;  // path_tracer.c, vkpt_pt_init_cvars
extern cvar_t *cvar_pt_ptlas;                                       // ...the same

/* ------------------------------------------------------------------------- */
/* vkpt_prep_backend for D3D12                                                */
/* ------------------------------------------------------------------------- */

// the light counts the light list builders write per cluster; nothing reads
// them on the GPU yet (the lighting passes will), so they stay on the CPU
static uint32_t *light_counts;
static int light_counts_size;

static void *d3d12_map_staging(vkpt_staging_t which)
{
    switch (which) {
    case VKPT_STAGING_LIGHTS:       return d3d12_world_staging(D3D12_WORLD_STAGING_LIGHTS);
    case VKPT_STAGING_IQM_MATRICES: return d3d12_world_staging(D3D12_WORLD_STAGING_IQM);
    case VKPT_STAGING_READBACK:     return d3d12_world_staging(D3D12_WORLD_STAGING_READBACK);
    case VKPT_STAGING_LIGHT_COUNTS: return light_counts;
    }
    return NULL;
}

static void d3d12_unmap_staging(vkpt_staging_t which)
{
    (void)which;    // persistently mapped
}

// vkpt_vertex_buffer_ensure_primbuf_size: the frame's models do not fit in the
// instanced buffers, which grow by vkpt's rule
static void d3d12_ensure_primbuf_size(uint32_t prim_count)
{
    if (prim_count <= d3d12_world_primbuf_size())
        return;
    vkpt_primbuf_grow(prim_count);
    d3d12_world_set_primbuf_size(vkpt_primbuf_size());
    vkpt_primbuf_new_generation();      // blood's device copy went with the old buffers
}

// vkpt_physical_sky_load_preset: SkyLoadScatterParameters' choice, world.c's tables
static void d3d12_load_sky_preset(int preset)
{
    const char *planet = NULL;
    const struct AtmosphereParameters *atmosphere = SkySelectPreset((SkyPreset)preset, &planet);
    d3d12_world_sky_load_preset(atmosphere, atmosphere ? sizeof(*atmosphere) : 0, planet);
}

static void d3d12_upload_envmap(int w, int h, byte *data)
{
    d3d12_world_upload_envmap(w, h, data);
}

static void d3d12_load_world(bsp_mesh_t *mesh, bsp_t *bsp)
{
    Z_Free(light_counts);
    light_counts_size = max(mesh->num_clusters, 1);
    light_counts = Z_Mallocz(sizeof(uint32_t) * light_counts_size);

    // vkpt_vertex_buffer_upload_bsp_mesh's layout, without the BLAS memory
    const uint64_t primitives_size = (uint64_t)mesh->num_primitives * sizeof(VboPrimitive);
    mesh->vertex_data_offset = primitives_size;
    const uint64_t size = primitives_size + (uint64_t)mesh->num_primitives * sizeof(prim_positions_t);
    uint8_t *data = Z_Malloc(size);
    memcpy(data, mesh->primitives, primitives_size);
    prim_positions_t *positions = (prim_positions_t *)(data + primitives_size);
    for (uint32_t prim = 0; prim < mesh->num_primitives; ++prim) {
        VectorCopy(mesh->primitives[prim].pos0, positions[prim][0]);
        VectorCopy(mesh->primitives[prim].pos1, positions[prim][1]);
        VectorCopy(mesh->primitives[prim].pos2, positions[prim][2]);
    }

    const int num_geoms = 5 + mesh->num_models;
    model_geometry_t **geoms = Z_Malloc(sizeof(*geoms) * num_geoms);
    geoms[0] = &mesh->geom_opaque;
    geoms[1] = &mesh->geom_transparent;
    geoms[2] = &mesh->geom_masked;
    geoms[3] = &mesh->geom_sky;
    geoms[4] = &mesh->geom_custom_sky;
    for (int i = 0; i < mesh->num_models; i++)
        geoms[5 + i] = &mesh->models[i].geometry;

    d3d12_world_blas_t *blases = Z_Mallocz(sizeof(*blases) * num_geoms);
    for (int i = 0; i < num_geoms; i++) {
        blases[i].prim_offsets = geoms[i]->prim_offsets;
        blases[i].prim_counts = geoms[i]->prim_counts;
        blases[i].num_geometries = geoms[i]->num_geometries;
    }

    d3d12_world_map_t map = {
        .data = data,
        .size = size,
        .primitives_size = primitives_size,
        .positions_offset = mesh->vertex_data_offset,
        .blases = blases,
        .num_blases = num_geoms,
        // vkpt_light_buffers_create's size
        .light_stats_count = max((uint64_t)mesh->num_clusters * (uint64_t)mesh->num_light_polys * 6 * 2, 1),
    };
    d3d12_world_load(&map);

    // what vkpt_pt_instance_model_blas reads (vkpt_model_geometry_has_blas:
    // with no Vulkan device, the address alone)
    for (int i = 0; i < num_geoms; i++)
        geoms[i]->blas_device_address = blases[i].address;
    vkpt_bsp_mesh_set_instance_flags(mesh);

    Z_Free(blases);
    Z_Free(geoms);
    Z_Free(data);

    // vulkan_load_world's vkpt_sky_exposure_begin_map: the map's sky exposure grid
    // and its solid mask, for world.c's buffer
    uint32_t *mask = Z_Malloc(SKY_EXPOSURE_MASK_WORDS * sizeof(uint32_t));
    if (vkpt_sky_exposure_fit_map(bsp, &mesh->world_aabb, d3d12_world_fog_ready() ? mask : NULL))
        d3d12_world_sky_exposure_mask(mask, SKY_EXPOSURE_MASK_WORDS);
    Z_Free(mask);
}

/* ------------------------------------------------------------------------- */
/* DLSS                                                                       */
/* ------------------------------------------------------------------------- */

/* NGX on this device (ngx.c), in DLSS.c's place: DLSSAdoptParameters hands it the
   capability block, so CheckSupport, the availability cvars and DLSSDescribeFeature
   read this instance's. Started on the first frame pt_dlss asks for it - vkpt starts
   its instance with the device when pt_dlss is on, and a pt_dlss change restarts the
   renderer; here it does not, so that frame may be any frame. */
static NVSDK_NGX_Parameter  *ngx_params;
static NVSDK_NGX_Handle     *ngx_feature;
static bool                 ngx_start_failed;   // until pt_dlss is changed again
static wchar_t              *ngx_dll_path, *ngx_data_path;  // NGX may keep them
// what ngx_feature was made for: remade when any of it changes (vkpt remakes it after
// the restart a pt_dlss change does, or when pt_dlss_dldn's callback says)
static struct {
    DLSSRenderResolution        res;
    bool                        rr;
    NVSDK_NGX_PerfQuality_Value quality;
} ngx_made;

static void dlss_release_feature(void)
{
    if (!ngx_feature)
        return;
    NVSDK_NGX_Result r = d3d12_ngx_release(ngx_feature);
    if (NVSDK_NGX_FAILED(r))
        Com_EPrintf("DLSS: NVSDK_NGX_D3D12_ReleaseFeature fail: %d\n", (int)r);
    ngx_feature = NULL;
}

// TryInit and CheckSupport, on this device; false (and said why) when NGX is not up
static bool dlss_start(void)
{
    char *folder = GetFolderPath();
    ngx_dll_path = (wchar_t *)GetWC(folder);
    free(folder);
    ngx_data_path = (wchar_t *)GetWC("DLSSTemp/");

    NVSDK_NGX_Result r = d3d12_ngx_init((ID3D12Device *)d3d.device, DLSS_PROJECT_ID, LONG_VERSION_STRING,
                                        ngx_dll_path, ngx_data_path, DLSSPrintCallback, &ngx_params);
    if (NVSDK_NGX_FAILED(r)) {
        Com_EPrintf("DLSS: NGX did not start on this D3D12 device: 0x%08x%s\n", (unsigned)r,
                    d3d.caps.sdk_version && !d3d.caps.process_sdk
                        ? " (an Agility SDK factory's device: NGX refuses those - see device.c)" : "");
        ngx_params = NULL;
        return false;
    }
    DLSSAdoptParameters(ngx_params);
    Com_Printf("D3D12: NGX up - Super Resolution %s, Ray Reconstruction %s\n",
               Cvar_VariableInteger("pt_dlss_available") ? "available" : "NOT available",
               Cvar_VariableInteger("pt_dlss_dldn_available") ? "available" : "NOT available");
    // what frame generation may offer (dlssgMaxGeneratedFrames), as vkpt reports it
    ReportFrameGenSupport();
    return true;
}

/* DLSS 5 (vkpt/DLSSNR.c) on this instance: its API half. The cvars, the feature's
   rebuilds, what the model is told and dlss5_info are DLSSNR.c's; ngx.c makes the calls.
   No pt_dlss5_drugs here (its loop is copies of Vulkan images). */
static NVSDK_NGX_Parameter *dlss5_instance_params(void)
{
    return ngx_params;
}

static NVSDK_NGX_Result dlss5_create(void *cmd, NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **feature)
{
    return d3d12_ngx_dlss5_create((ID3D12GraphicsCommandList *)cmd, params, feature);
}

static const dlss5_backend_t d3d12_dlss5_backend = {
    .instance_params = dlss5_instance_params,
    .new_params = d3d12_ngx_new_params,
    .destroy_params = d3d12_ngx_destroy_params,
    .create = dlss5_create,
    .release = d3d12_ngx_release,
    .wait_idle = d3d12_wait_idle,
    .feedback = qfalse,
};

static void dlssg_release_feature(void);

// DLSSDeconstructor's part: the GPU is idle
static void dlss_stop(void)
{
    // DLSSDeconstructor's order: DLSS 5's feature and block (they live on the
    // instance), frame generation's feature, then the instance
    DLSS5_ReleaseNGX();
    dlssg_release_feature();
    dlss_release_feature();
    qvk.supports_ngx = false;
    DLSSAdoptParameters(NULL);
    if (ngx_params)
        d3d12_ngx_shutdown((ID3D12Device *)d3d.device, ngx_params);
    ngx_params = NULL;
    free(ngx_dll_path);
    free(ngx_data_path);
    ngx_dll_path = ngx_data_path = NULL;
    ngx_start_failed = false;
}

// pt_dlss: DLSSModeSelected (no vid_rtx restart - the images follow the extents
// each frame), and NGX may be tried again
static void dlss_mode_selected(cvar_t *self)
{
    DLSSModeSelected(self);
    ngx_start_failed = false;

    // NGX does not start on a device factory's device (device.c): a renderer restart
    // makes the device again, on the system runtime now that pt_dlss is on. The
    // process's own Agility runtime (the export) needs none.
    if (self->integer && d3d.caps.sdk_version && !d3d.caps.preview_sdk && !d3d.caps.process_sdk) {
        Com_Printf("D3D12: DLSS needs the system D3D12 runtime - restarting the renderer\n");
        Cbuf_AddText(&cmd_buffer, "vid_restart\n");
    }
}

// this frame's vkpt_prepare_frame result, for d3d12_frontend_finish_frame (and the
// DLSS record's history reset: the model view's photo-mode rule)
static vkpt_frame_prep_t frame_prep;
static bool frame_prep_valid;

void d3d12_frontend_dlss_record(void *ctx, ID3D12GraphicsCommandList4 *cmd)
{
    (void)ctx;
    if (!ngx_params)
        return;
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cmd;

    // DLSSApply's sizes: the frame's, and the feature's - made for the largest
    // render extent, the frame's a sub-rectangle of it (DLSSFeatureResolution), so
    // a DRS step or the way out of photo mode keeps it
    DLSSRenderResolution res = {
        .inputWidth = (int)qvk.extent_render.width,
        .inputHeight = (int)qvk.extent_render.height,
        .outputWidth = (int)qvk.extent_unscaled.width,
        .outputHeight = (int)qvk.extent_unscaled.height,
    };
    DLSSClampResolution(&res);
    DLSSRenderResolution feature_res = res;
    DLSSFeatureResolution(&feature_res);

    // ValidateDLSSFeature: remade when it was made for anything else; frames in
    // flight may still be using the old one
    const bool rr = DLSSModeDenoise() == 1;
    const NVSDK_NGX_PerfQuality_Value quality = ToNGXPerfQuality();
    if (ngx_feature && (memcmp(&feature_res, &ngx_made.res, sizeof(feature_res)) || ngx_made.rr != rr ||
                        ngx_made.quality != quality)) {
        d3d12_wait_idle();
        dlss_release_feature();
    }
    if (!ngx_feature) {
        NVSDK_NGX_DLSS_Create_Params sr;
        NVSDK_NGX_DLSSD_Create_Params rr_create;
        const bool made_rr = DLSSDescribeFeature(feature_res, ngx_params, &sr, &rr_create);
        NVSDK_NGX_Result r = d3d12_ngx_create(gl, ngx_params, made_rr, &sr, &rr_create, &ngx_feature);
        if (NVSDK_NGX_FAILED(r)) {
            Com_EPrintf("DLSS: %s feature create failed: 0x%08x - see DLSSTemp/nvngx*.log\n",
                        made_rr ? "RayReconstruction" : "SuperSampling", (unsigned)r);
            ngx_feature = NULL;
            DLSSFeatureFailed();
            return;
        }
        ngx_made.res = feature_res;
        ngx_made.rr = made_rr;
        ngx_made.quality = quality;
    }

    DLSSEvaluation ev;
    DLSSDescribeEvaluation(vkpt_refdef.uniform_buffer.sub_pixel_jitter, &ev);
    d3d12_ngx_eval_t e = {
        .rr = ngx_made.rr,
        .color = d3d12_world_image(ev.color_image),
        .output = d3d12_world_image(VKPT_IMG_DLSS_OUTPUT),
        .depth = d3d12_world_image(VKPT_IMG_DLSS_DEPTH),
        .motion = d3d12_world_image(VKPT_IMG_PT_DLSS_MOTION),
        .diffuse_albedo = d3d12_world_image(VKPT_IMG_DLSS_ALBEDO),
        .specular_albedo = d3d12_world_image(VKPT_IMG_DLSS_SPECULAR_ALBEDO),
        .normals = d3d12_world_image(VKPT_IMG_DLSS_NORMAL),
        .roughness = d3d12_world_image(VKPT_IMG_DLSS_ROUGHNESS),
        .specular_hit_distance = d3d12_world_image(VKPT_IMG_DLSS_RAYLENGTH_SPECULAR),
        .reflection_motion = ev.reflection_motion ? d3d12_world_image(VKPT_IMG_DLSS_REFLECT_MOTION) : NULL,
        .diffuse_hit_distance = ev.diffuse_hit_distance ? d3d12_world_image(VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE) : NULL,
        .reflected_albedo = ev.reflected_albedo ? d3d12_world_image(VKPT_IMG_DLSS_REFLECTED_ALBEDO) : NULL,
        .before_transparency = ev.before_transparency ? d3d12_world_image(VKPT_IMG_DLSS_BEFORE_TRANSPARENT) : NULL,
        .responsivity_mask = ev.responsivity_mask ? d3d12_world_image(VKPT_IMG_DLSS_RESPONSIVITY) : NULL,
        .width = (uint32_t)res.inputWidth,
        .height = (uint32_t)res.inputHeight,
        .jitter_x = ev.jitter_x,
        .jitter_y = ev.jitter_y,
        .reset = vkpt_dlss_take_history_reset(!frame_prep_valid || frame_prep.render_world),
        .matrices = ev.matrices != 0,
    };
    memcpy(e.world_to_view, ev.world_to_view, sizeof(e.world_to_view));
    memcpy(e.view_to_clip, ev.view_to_clip, sizeof(e.view_to_clip));

    NVSDK_NGX_Result r = d3d12_ngx_evaluate(gl, ngx_feature, ngx_params, &e);
    static bool reported_eval_failure = false;
    if (NVSDK_NGX_FAILED(r) && !reported_eval_failure) {
        Com_WPrintf("DLSS%s evaluate failed: 0x%08x (reported once)\n", e.rr ? "-RR" : "", (unsigned)r);
        reported_eval_failure = true;
    }
}

bool d3d12_frontend_dlss5_record(void *ctx, ID3D12GraphicsCommandList4 *cmd, int stage)
{
    (void)ctx;
    if (!ngx_params)
        return false;
    // DLSS5Apply's sizes: the output, and the guides at the render size
    if (stage == 0)
        return DLSS5Prepare(cmd, qvk.extent_unscaled.width, qvk.extent_unscaled.height) != qfalse;

    NVSDK_NGX_Handle *feature = NULL;
    NVSDK_NGX_Parameter *params = DLSS5DescribeEvaluation(vkpt_dlss5_take_history_reset() ? qtrue : qfalse,
                                                          qvk.extent_render.width, qvk.extent_render.height,
                                                          &feature);
    if (!feature || !params)
        return false;
    const NVSDK_NGX_Result r = d3d12_ngx_dlss5_evaluate(
        (ID3D12GraphicsCommandList *)cmd, feature, params, d3d12_world_image(VKPT_IMG_DLSS_OUTPUT),
        d3d12_world_image(VKPT_IMG_DLSS_DEPTH), d3d12_world_image(VKPT_IMG_PT_DLSS_MOTION),
        d3d12_world_image(VKPT_IMG_MOTION_BLUR));
    return DLSS5Evaluated(r) != qfalse;
}

/* ------------------------------------------------------------------------- */
/* FRAME GENERATION                                                           */
/* ------------------------------------------------------------------------- */

/* DLSSGApply through NGX's D3D12 entry points (ngx_dlssg.cpp), on the instance above,
   and what R_EndFrame_RTX does with the frames: the presents go to fg_present.c's
   thread, which paces them onto swapchain.c's present queue.

   The feature is made for the output size, the largest render size the frame can
   have (DLSSFeatureResolution, with DynamicResolutionScaling - dlssg_ngx.hpp) and the
   number of generated frames - a create-time property (ValidateDLSSGFeature's rule).
   It is remade when the output or the count changes, or a frame is larger than it. What each evaluation is told is DLSS.c's (DLSSGDescribeEvaluation),
   as vkpt tells it. */
static NVSDK_NGX_Handle *dlssg_feature;
static struct {
    uint32_t        width, height, render_width, render_height;
    unsigned int    generated;
} dlssg_made;
static uint64_t     dlssg_last_frame;   // d3d.frame_counter + 1 of the last evaluated frame
static unsigned int fg_last_group;      // presents the previous frame queued
static uint64_t     fg_group_id;

static void dlssg_release_feature(void)
{
    if (!dlssg_feature)
        return;
    NVSDK_NGX_Result r = d3d12_ngx_release(dlssg_feature);
    if (NVSDK_NGX_FAILED(r))
        Com_EPrintf("DLSS-G: NVSDK_NGX_D3D12_ReleaseFeature fail: %d\n", (int)r);
    dlssg_feature = NULL;
}

// DLSSGApply: `generated` evaluations into DLSS_FG_OUTPUT..; how many were made
static unsigned int dlssg_evaluate(ID3D12GraphicsCommandList4 *cmd, unsigned int generated)
{
    ID3D12GraphicsCommandList *gl = (ID3D12GraphicsCommandList *)cmd;
    const uint32_t width = qvk.extent_unscaled.width, height = qvk.extent_unscaled.height;
    const uint32_t render_width = qvk.extent_render.width, render_height = qvk.extent_render.height;
    // a frame that is not the next one after the last evaluated (photo mode, a menu
    // in between) starts the history again too
    bool reset = vkpt_dlssg_take_history_reset() || dlssg_last_frame != d3d.frame_counter;
    // vid_hdr switched: the frames it saw were in the other curve
    if (qvk.surf_is_hdr != d3d.hdr)
        reset = true;
    qvk.surf_is_hdr = d3d.hdr;

    // frames in flight may still be generating with the old feature. One whose render
    // extent covers this frame's stays: a DRS step remakes nothing
    if (dlssg_feature && (dlssg_made.width != width || dlssg_made.height != height ||
                          dlssg_made.render_width < render_width || dlssg_made.render_height < render_height ||
                          dlssg_made.generated != generated)) {
        d3d12_wait_idle();
        dlssg_release_feature();
    }
    if (!dlssg_feature) {
        // made for the largest render extent the frame can have, as Super Resolution's
        struct DLSSRenderResolution made = { (int)render_width, (int)render_height, (int)width, (int)height };
        DLSSFeatureResolution(&made);
        // the backbuffer handed over is DLSS_OUTPUT, RGBA16F
        NVSDK_NGX_Result r = d3d12_ngx_dlssg_create(gl, ngx_params, width, height, (uint32_t)made.inputWidth,
                                                    (uint32_t)made.inputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                    generated, &dlssg_feature);
        if (NVSDK_NGX_FAILED(r)) {
            Com_EPrintf("DLSS-G: feature create failed: 0x%08x - see DLSSTemp/nvngx_dlssg*.log\n", (unsigned)r);
            dlssg_feature = NULL;
            DLSSGFeatureFailed();
            return 0;
        }
        dlssg_made.width = width;
        dlssg_made.height = height;
        dlssg_made.render_width = (uint32_t)made.inputWidth;
        dlssg_made.render_height = (uint32_t)made.inputHeight;
        dlssg_made.generated = generated;
        Com_Printf("DLSS-G: D3D12 feature %ux%u from up to %ux%u, %ux\n", width, height, dlssg_made.render_width,
                   dlssg_made.render_height, generated + 1);
        reset = true;
    }

    DLSSGEvaluation ev;
    DLSSGDescribeEvaluation(reset ? qtrue : qfalse, &ev);

    // dlssg_hdr.c (vkpt_dlssg_hdr_active): DLSS-G is handed the display-encoded frame
    // it asks for - HDR10/PQ on an HDR swapchain (modes 0, 1), sRGB on an SDR one
    // (2, 3) - and everything after it gets linear back
    static cvar_t *fg_srgb, *fg_hdr10;
    if (!fg_srgb) {
        fg_srgb = Cvar_Get("pt_dlss_fg_srgb", "1", 0);
        fg_hdr10 = Cvar_Get("pt_dlss_fg_hdr10", "1", 0);
    }
    const bool encode = (d3d.hdr ? fg_hdr10 : fg_srgb)->integer != 0;
    const uint32_t curve = d3d.hdr ? 0 : 2;
    if (encode)
        d3d12_world_dlssg_convert(cmd, curve, 0);
    else
        d3d12_barrier_all(cmd, d3d.caps.enhanced_barriers);    // DLSS_OUTPUT's writes and the blit's read

    d3d12_ngx_dlssg_eval_t e = {
        .backbuffer = d3d12_world_image(VKPT_IMG_DLSS_OUTPUT),
        .depth = d3d12_world_image(ev.depth_image),
        .motion = d3d12_world_image(VKPT_IMG_PT_DLSS_MOTION),
        .desc = &ev.desc,
        .frame_id = d3d.frame_counter,
    };
    // the HUD-less hint: see DLSSGDescribeEvaluation
    e.hudless = ev.hudless ? e.backbuffer : NULL;

    unsigned int made = 0;
    for (unsigned int k = 1; k <= generated; k++) {
        ev.desc.multiFrameIndex = k;
        e.output = d3d12_world_image(DLSSGOutputImageIndex(k));
        NVSDK_NGX_Result r = d3d12_ngx_dlssg_evaluate(gl, dlssg_feature, ngx_params, &e);
        if (NVSDK_NGX_FAILED(r)) {
            // structural rather than transient, as DLSSGApply treats it
            Com_EPrintf("DLSS-G: evaluate failed on generated frame %u/%u: 0x%08x"
                        " - see DLSSTemp/nvngx_dlssg*.log\n", k, generated, (unsigned)r);
            DLSSGFeatureFailed();
            break;
        }
        made = k;
    }

    // linear again: the real frame (bloom and DLSS read it next frame) and each generated one
    if (encode) {
        d3d12_world_dlssg_convert(cmd, curve + 1, 0);
        for (unsigned int k = 1; k <= made; k++)
            d3d12_world_dlssg_convert(cmd, curve + 1, k);
    }
    dlssg_last_frame = d3d.frame_counter + 1;
    return made;
}

unsigned int d3d12_frontend_dlssg_record(ID3D12GraphicsCommandList4 *cmd)
{
    unsigned int generated = 0;
    // photo mode presents every render once (vkpt_fg_bypassed); and the swapchain
    // must be on the present queue already - it moves at the next R_BeginFrame
    if (ngx_params && d3d12_fg_mode_active() && DLSSGEnabled() && !vkpt_fg_bypassed())
        generated = min(DLSSGGeneratedFrames(), D3D12_FG_MAX_GENERATED);
    if (generated && !d3d12_fg_sources((int)generated))
        generated = 0;
    if (generated)
        generated = dlssg_evaluate(cmd, generated);
    // the FPS readout's presents per render (CL_GetPresentedFramesPerRender)
    if (vkpt_refdef.fd)
        vkpt_refdef.fd->feedback.presented_frames = (int)generated + 1;
    return generated;
}

/* fg_present.c's thread presents through these: readiness is swapchain.c's fence
   that the main queue signals after each frame, and the item's image_index names
   the present source - set * 8 + index (0 the real frame). The real frame's item
   carries its Reflex frame id (0 without Reflex), and its PRESENT_START / PRESENT_END
   markers are set here, as fg_present.c's Vulkan present sets them. */
static bool fg_backend_ready_wait(uint64_t value)
{
    return d3d12_fg_ready_wait(value);
}

static bool fg_backend_ready_passed(uint64_t value)
{
    return d3d12_fg_ready_passed(value);
}

static VkResult fg_backend_present(const fg_present_item_t *item, bool image_ready)
{
    (void)image_ready;  // the present queue waits for the frame on the GPU either way
    if (item->reflex_present_id)
        d3d12_reflex_marker_for_frame(D3D12_REFLEX_PRESENT_START, item->reflex_present_id);
    const bool ok = d3d12_fg_present((int)(item->image_index >> 3), (int)(item->image_index & 7),
                                     item->timeline_value);
    if (item->reflex_present_id)
        d3d12_reflex_marker_for_frame(D3D12_REFLEX_PRESENT_END, item->reflex_present_id);
    return ok ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}

static const fg_present_backend_t d3d12_fg_backend = {
    .ready_wait = fg_backend_ready_wait,
    .ready_passed = fg_backend_ready_passed,
    .present = fg_backend_present,
};

// pt_vrr's override (Reflex_RegisterCvars registers it, from d3d12_reflex_init too)
// or what NVAPI said for this window's display (Reflex_DetectVRR)
static bool fg_display_vrr(void)
{
    cvar_t *vrr = Cvar_Get("pt_vrr", "-1", CVAR_ARCHIVE);
    return vrr->integer >= 0 ? vrr->integer != 0 : Reflex_DisplayVRR();
}

static void fg_stop(void)
{
    FGPresent_Shutdown();
    FGPresent_SetBackend(NULL);     // the Vulkan renderer's again
}

void d3d12_frontend_fg_begin_frame(void)
{
    // NGX starts with the first frame that wants DLSS, and DLSSGEnabled() follows it
    const bool want = ngx_params && DLSSGEnabled();
    if (want == d3d12_fg_mode_active())
        return;

    if (!want) {
        FGPresent_Drain();
        d3d12_fg_mode(false);
        fg_stop();
        fg_last_group = 0;
        return;
    }

    if (!d3d12_fg_mode(true)) {
        // as after a failed feature: off until the renderer starts again
        Com_EPrintf("DLSS-G: no present queue for frame generation - it stays off\n");
        DLSSGFeatureFailed();
        return;
    }
    FGPresent_SetBackend(&d3d12_fg_backend);
    FGPresent_Init();
    fg_last_group = 0;
    // R_EndFrame_RTX's VRR answer, for the pacing below (vkpt asks at swapchain creation)
    d3d12_frontend_detect_vrr();
}

void d3d12_frontend_detect_vrr(void)
{
    char gdi[64];
    if (vkpt_get_window_gdi_device(gdi, sizeof(gdi)))
        Reflex_DetectVRR(gdi);
}

// vid_present_stats: frame generation's wait for the present thread, below, which
// counts as the presentation's (the next report takes it)
static uint64_t fg_present_wait_us;

void d3d12_frontend_fg_before_submit(int set)
{
    const uint64_t t0 = vkpt_present_stats_enabled() ? Sys_Microseconds() : 0;
    // The sources of `set` were last written two frames ago. Once at most the previous
    // frame's presents are queued, all of that frame's have been issued - and the
    // main queue can wait for their copies out of the sources.
    FGPresent_WaitUntilPending(fg_last_group);
    d3d12_fg_wait_set(set);
    if (t0)
        fg_present_wait_us += Sys_Microseconds() - t0;
}

// R_EndFrame_RTX's tm_debug line (the histogram and the curve are tone_mapping_apply's)
extern cvar_t *cvar_tm_debug;

void d3d12_frontend_draw_debug(void)
{
    if (cvar_tm_debug && cvar_tm_debug->integer)
        vkpt_tone_mapping_draw_debug();
}

// pt_dlss_fg_compare and pt_dlss_fg_dump (vkpt main.c's), over this frame's images
void d3d12_frontend_fg_debug(unsigned int generated)
{
    if (vkpt_fg_debug_wanted())
        vkpt_fg_debug_frames(generated, d3d12_world_read_image);
}

// pt_dlss_fg_show 1 (vkpt's DLSSGShowInterpolated, which asks Vulkan's feature): the first
// generated frame on screen in the real frame's place
bool d3d12_frontend_fg_show_generated(void)
{
    static cvar_t *show;
    if (!show)
        show = Cvar_Get("pt_dlss_fg_show", "0", 0);
    return show->integer != 0;
}

bool d3d12_frontend_present_stats_on(void)
{
    return vkpt_present_stats_enabled();
}

/* vid_present_stats, vkpt's report: this renderer blocks for a frame at its slot's
   fence and for the presentation - the swapchain's latency wait, or with frame
   generation the present thread (fg_present_wait_us). */
void d3d12_frontend_present_stats(uint64_t fence_us, uint64_t present_wait_us)
{
    static cvar_t *vid_vsync;
    if (!vid_vsync)
        vid_vsync = Cvar_Get("vid_vsync", "0", CVAR_ARCHIVE);
    const char *mode = d3d12_fg_mode_active() ? "FG present thread"
                     : vid_vsync->integer      ? "vsync"
                     : d3d.tearing_enabled     ? "tearing"
                                               : "no vsync, no tearing";
    vkpt_present_stats_frame(fence_us, present_wait_us + fg_present_wait_us, mode, (uint32_t)d3d.swapchain_images);
    fg_present_wait_us = 0;
}

void d3d12_frontend_fg_present(int set, unsigned int generated)
{
    const uint64_t ready = d3d12_fg_signal_ready();
    const unsigned int total = generated + 1;

    /* R_EndFrame_RTX's relative pacing (fg_present.h, FGPresent_Enqueue): the group
       spread over the measured render cadence from the moment it is ready, `pace` of
       it - vkpt takes this path with vsync off or on a VRR display. Here it is taken
       always: with vsync on, the present queue's FIFO and the swapchain's latency
       wait do the rest (vkpt's absolute schedule is not ported). */
    const bool vrr = fg_display_vrr();
    float pace = vrr ? Cvar_Get("pt_dlss_fg_vrr_pace", "1.0", CVAR_ARCHIVE)->value
                     : Cvar_Get("pt_dlss_fg_pace", "0.95", CVAR_ARCHIVE)->value;
    clamp(pace, 0.5f, 1.0f);
    const int refresh_hz = Reflex_DisplayRefreshHz();
    // VRR's floor: the panel's fastest refresh
    const uint64_t min_gap_us = (vrr && refresh_hz > 0) ? (uint64_t)(1000000 / refresh_hz) : 0;

    fg_group_id++;
    for (unsigned int i = 0; i < total; i++) {
        // the generated frames sit between the previous real frame and this one
        const unsigned int source = i < generated ? i + 1 : 0;
        // the real frame carries its Reflex frame id: its present is marked by it
        const uint64_t reflex_id = source == 0 ? d3d12_reflex_frame_id() : 0;
        if (!FGPresent_Enqueue(VK_NULL_HANDLE, (uint32_t)(set * 8 + (int)source), VK_NULL_HANDLE, 0, min_gap_us,
                               reflex_id, fg_group_id, ready, i, total, pace)) {
            // no thread (it could not start) or a full queue: once nothing is queued,
            // present from here
            FGPresent_Drain();
            if (reflex_id)
                d3d12_reflex_marker_for_frame(D3D12_REFLEX_PRESENT_START, reflex_id);
            d3d12_fg_present(set, (int)source, ready);
            if (reflex_id)
                d3d12_reflex_marker_for_frame(D3D12_REFLEX_PRESENT_END, reflex_id);
        }
    }
    fg_last_group = total;
}

void d3d12_frontend_fg_drain(void)
{
    FGPresent_Drain();
}

/* ------------------------------------------------------------------------- */

/* NVIDIA Flow (vkpt/flow.c) on this device: Flow's own D3D12 backend executes on the
   frame's queue, and flow.c's Vulkan half is flow.c's here */
static bool d3d12_flow_backend_create(size_t params_stride, size_t light_sums_stride, size_t occupancy_stride,
                                      byte **params_mapped, byte **light_sums_mapped)
{
    return d3d12_flow_create(params_stride, light_sums_stride, occupancy_stride, params_mapped, light_sums_mapped);
}

static void d3d12_flow_backend_bind(int set, void *density, uint32_t density_format, void *table)
{
    d3d12_flow_bind(set, density, (DXGI_FORMAT)density_format, table);
}

static vkpt_flow_backend_t d3d12_flow_backend = {
    .create = d3d12_flow_backend_create,
    .destroy = d3d12_flow_destroy,
    .create_env = d3d12_flow_create_env,
    .destroy_env = d3d12_flow_destroy_env,
    .bind = d3d12_flow_backend_bind,
    .profile = d3d12_timers_flow,
};

/* `mat reload` / `mat <attribute>` (vkpt_vertex_buffer_invalidate_static_model_vbos):
   a static model whose material changed is let go here, outside any frame, and the
   next frame's d3d12_frontend_upload_models sees it as VKPT_MODEL_VBO_UPLOAD (meshes,
   no VBO) and builds it again - with the mesh reclassified (opaque / transparent /
   masked) from the new material, which is why the BLASes cannot simply be kept.
   d3d12_world_model_free defers the release anyway. */
static void d3d12_backend_free_model(int index)
{
    d3d12_world_model_free(index);
    vkpt_model_vbo_clear(index);
}

static const vkpt_prep_backend_t d3d12_backend = {
    .map_staging = d3d12_map_staging,
    .unmap_staging = d3d12_unmap_staging,
    .ensure_primbuf_size = d3d12_ensure_primbuf_size,
    .load_sky_preset = d3d12_load_sky_preset,
    .upload_envmap = d3d12_upload_envmap,
    .load_world = d3d12_load_world,
    .reserve_blood_staging = d3d12_world_reserve_blood_staging,
    .create_froxel_volumes = d3d12_world_froxel_create,
    .read_sky_envmap = d3d12_world_read_sky_envmap,
    .wait_idle = d3d12_wait_idle,
    .free_model = d3d12_backend_free_model,
};

/* ------------------------------------------------------------------------- */

void d3d12_frontend_init(void)
{
    // every cvar the frame's CPU half and the materials read, the md2 normal
    // table, the TAA jitter, MAT_Init - what R_Init_RTX runs before its device
    vkpt_init_frontend();
    vkpt_prep_backend = &d3d12_backend;
    // DLSS 5 (DLSSNR.c) through this renderer's NGX instance; pt_dlss5_available is
    // what DLSS5_InitCvars found (nvngx_dlssnr.dll next to the exe)
    DLSS5_SetBackend(&d3d12_dlss5_backend);
    // no froxel volumes of this renderer's yet, no sky exposure bake
    vkpt_god_rays_cpu_reset();
    // The shared code reads qvk.device != NULL as "Vulkan is running"
    // (vkpt_model_geometry_has_blas, the froxel volumes, registration's idle
    // wait). Vulkan is shut down whenever this renderer starts; make sure no
    // handle of it survives - a stale one aborts the game in the Vulkan loader.
    qvk.device = VK_NULL_HANDLE;
    // DLSSEnabled() is true while pt_dlss is set on a device that has NGX - and
    // a Vulkan session before this one leaves that flag up. It is this device's
    // to say: d3d12_frontend_world_init sets it for an NVIDIA adapter.
    qvk.supports_ngx = false;
    // pt_dlss's callback (viewsize_changed) restarts the renderer with vid_rtx 0
    // then 1 - which would leave D3D12 for Vulkan. R_Init_RTX puts it back.
    cvar_t *pt_dlss = Cvar_FindVar("pt_dlss");     // vkpt_init_frontend made it
    if (pt_dlss)
        pt_dlss->changed = dlss_mode_selected;
    // one GPU: the frame's CPU half divides by it
    qvk.device_count = 1;
    // Frame generation: DLSS.c's display questions (Reflex_DisplayRefreshHz, the
    // window's monitor) read the window from qvk; whether the swapchain is HDR
    // (DLSSGDescribeEvaluation's colorBuffersHDR) is set at each evaluation, from
    // d3d.hdr; no generated-frame budget is set here, so none left by a Vulkan
    // session may hold
    qvk.window = sdl_window;
    qvk.surf_is_hdr = false;
    DLSSGSetFrameBudget(0);
    // fresh staging buffers: the first light upload is whole
    vkpt_upload_ranges_reset();

    // MAT_Init's `mat` command stays: its model rebuild goes through the backend's
    // wait_idle / free_model (d3d12_backend above), the crosshair's material through
    // the shared readback (process_render_feedback in vkpt_prepare_frame)

    // restarts the renderer with vid_rtx 0 then 1 - which would leave D3D12
    // for Vulkan. The images follow the field layout's extents the next frame
    // (world.c's ensure_images), so nothing else is needed. R_Init_RTX puts the
    // callback back.
    Cvar_Get("pt_dlss_split_fields", "1", CVAR_ARCHIVE)->changed = NULL;

    // blood.c's CPU state and its cvars (vkpt_blood_initialize makes no Vulkan
    // object; the shadow copy comes with the first blood)
    vkpt_blood_initialize();

    // R_Init_RTX's console commands that need no device: fog, camera, cpu_bench,
    // fog_debug, profiler_dump, reload_textures, show_pvs, next_sun
    vkpt_register_commands();

    // the IQM matrices' CPU copies, which prepare_entities fills; vkpt makes
    // them in vkpt_vertex_buffer_create
    if (!qvk.iqm_matrices_shadow)
        qvk.iqm_matrices_shadow = Z_Mallocz(sizeof(IqmMatrixBuffer));
    if (!qvk.iqm_matrices_prev)
        qvk.iqm_matrices_prev = Z_Mallocz(sizeof(IqmMatrixBuffer));
}

void d3d12_frontend_shutdown(void)
{
    // the commands; cpu_bench's workers before the world mesh and the textures go
    vkpt_unregister_commands();
    // the present thread, its queue issued (the swapchain goes after this)
    fg_stop();
    fg_last_group = 0;
    dlssg_last_frame = 0;
    // DLSS 5 (its feature, its block, dlss5_info - R_Shutdown_RTX's DLSS5_Shutdown),
    // then the other features and the instance (R_Shutdown_D3D12 waited for the GPU);
    // DLSS 5's NGX calls are Vulkan's again after
    DLSS5_Shutdown();
    dlss_stop();
    DLSS5_SetBackend(NULL);
    // the world's mesh and BSP: the next renderer loads the map afresh
    vkpt_unload_world();
    // the model slots' CPU half, so the next renderer uploads every model
    // again (world.c lets go of the buffers at its shutdown)
    for (int i = 0; i < MAX_MODELS; i++)
        vkpt_model_vbo_clear(i);
    Z_Free(light_counts);
    light_counts = NULL;
    light_counts_size = 0;
    qvk.device_count = 0;

    // NVIDIA Flow: the simulation, then this renderer's half (R_Shutdown_D3D12 waited
    // for the GPU)
    if (vkpt_flow_backend) {
        vkpt_flow_destroy();
        vkpt_flow_backend = NULL;
    }

    // the particles', beams' and sprites' CPU frame (destroy_transparency's part)
    vkpt_transparency_cpu_shutdown();
    // blood.c's shadow copy (the Vulkan staging it would release was never made)
    vkpt_blood_destroy();
    // world.c's froxel volumes went with it
    vkpt_god_rays_cpu_reset();

    MAT_Shutdown();
    Z_Free(qvk.iqm_matrices_shadow);
    Z_Free(qvk.iqm_matrices_prev);
    qvk.iqm_matrices_shadow = NULL;
    qvk.iqm_matrices_prev = NULL;
}

void d3d12_frontend_mesh_shutdown(void)
{
    MOD_TraceMeshShutdown_RTX();
}

void d3d12_frontend_world_init(void)
{
    // vkpt's supports_ngx is the NVX extensions, i.e. an NVIDIA GPU; and what the
    // reconstruction menu may offer, NGX up or not (DLSSPublishAvailability)
    DXGI_ADAPTER_DESC3 adapter;
    qvk.supports_ngx = d3d.adapter && SUCCEEDED(IDXGIAdapter4_GetDesc3(d3d.adapter, &adapter)) &&
                       adapter.VendorId == 0x10de;
    DLSSAdoptParameters(NULL);

    // vkpt_initialize_all's images: the UBO names the water normal map by index
    vkpt_find_permanent_images();
    // vkpt_vertex_buffer_create's instanced buffers, pt_primbuf big
    d3d12_world_set_primbuf_size(vkpt_primbuf_size());
    vkpt_primbuf_new_generation();      // blood's device copy went with the old buffers

    // vkpt_physical_sky_initialize's part: world.c's tables are new, so the
    // next frame loads the preset into them and renders the sky
    vkpt_physical_sky_reset();
    if (!d3d12_world_ready())
        return;

    // NVIDIA Flow (vkpt_initialize_all's "flow"), on this device and the frame's
    // queue; its buffers and bindings are flow.c's of this renderer, in the world's
    // textures sets. pt_flow 0 or no libraries: the dummies, and no simulation.
    d3d12_flow_backend.d3d12_device = d3d.device;
    d3d12_flow_backend.d3d12_queue = d3d.queue;
    vkpt_flow_backend = &d3d12_flow_backend;
    vkpt_flow_initialize();

    // initialize_transparency: the CPU frame the particles, beams and sprites
    // are written into, and world.c's buffers sized as vkpt's, with the quads'
    // index pattern. Without them the effects TLAS stays empty (said why).
    if (vkpt_transparency_cpu_init()) {
        uint64_t capacity[D3D12_TR_NUM_STREAMS + 1];
        for (int i = 0; i <= D3D12_TR_NUM_STREAMS; i++)
            capacity[i] = vkpt_transparency_capacity(i);
        uint16_t *indices = Z_Malloc(capacity[D3D12_TR_INDICES]);
        vkpt_transparency_fill_indices(indices);
        d3d12_world_transparency_init(capacity, vkpt_transparency_frame_capacity(), indices);
        Z_Free(indices);
    }

    // InitializeShadowmapResources' grid
    float *vertices = NULL;
    uint32_t *indices = NULL;
    const uint32_t num_indices = SkyBuildTerrainGrid("env/terrain_heightmap.dds", ShadowmapGridSize,
                                                     ShadowmapWorldSize, &vertices, &indices);
    if (!num_indices)
        return;     // said why; the terrain then shadows nothing
    d3d12_world_sky_set_terrain(vertices, ShadowmapGridSize * ShadowmapGridSize, indices, num_indices);
    Z_Free(vertices);
    Z_Free(indices);
}

void d3d12_frontend_begin_registration(const char *map)
{
    // R_BeginRegistration_RTX: the map's cfgs, the BSP, its mesh and light
    // lists, the sky's settings - and d3d12_load_world for the GPU side
    vkpt_begin_registration(map);
}

void d3d12_frontend_end_registration(void)
{
    vkpt_end_registration();
}

void d3d12_frontend_upload_models(void)
{
    if (!d3d12_world_ready())
        return;

    bool changed = false;
    for (int i = 0; i < MAX_MODELS; i++) {
        switch (vkpt_model_vbo_action(i)) {
        case VKPT_MODEL_VBO_KEEP:
            continue;
        case VKPT_MODEL_VBO_FREE:
            d3d12_world_model_free(i);
            vkpt_model_vbo_clear(i);
            changed = true;
            continue;
        case VKPT_MODEL_VBO_UPLOAD:
            break;
        }

        // a slot in use is let go first (d3d12_world_model_upload does that too)
        d3d12_world_model_free(i);
        vkpt_model_vbo_clear(i);
        changed = true;

        model_vbo_t *vbo = &model_vertex_data[i];
        const size_t size = vkpt_model_vbo_layout(i);
        uint8_t *data = Z_Malloc(size);
        vkpt_model_vbo_stage(i, data, size);

        d3d12_world_model_t m = {
            .data = data,
            .size = size,
            .primitives_size = (uint64_t)vbo->total_tris * sizeof(VboPrimitive),
            .positions_offset = vbo->vertex_data_offset,
        };
        model_geometry_t *geoms[3] = { &vbo->geom_opaque, &vbo->geom_transparent, &vbo->geom_masked };
        if (vbo->is_static) {
            for (int k = 0; k < 3; k++) {
                m.blases[k].prim_offsets = geoms[k]->prim_offsets;
                m.blases[k].prim_counts = geoms[k]->prim_counts;
                m.blases[k].num_geometries = geoms[k]->num_geometries;
            }
            m.num_blases = 3;
        }
        const bool present = d3d12_world_model_upload(i, &m);
        Z_Free(data);

        // what vkpt_pt_instance_model_blas reads (vkpt_model_geometry_has_blas:
        // with no Vulkan device, the address alone)
        for (int k = 0; k < m.num_blases; k++)
            geoms[k]->blas_device_address = m.blases[k].address;
        vkpt_model_vbo_set_instance_flags(i);
        vkpt_model_vbo_mark_uploaded(i, present);
    }

    if (changed)
        d3d12_world_models_commit();
}

static bool frame_sky_update;       // the frame was asked to render the sky
static bool frame_took_bake;        // ...took a batch of the sky exposure bake
// shadow_map.c's instances, as the frame hands them to world.c
static d3d12_shadow_instance_t smap_instances[MAX_MODEL_INSTANCES];

void d3d12_frontend_finish_frame(bool drawn)
{
    if (frame_prep_valid)
        vkpt_finish_frame(&frame_prep);
    frame_prep_valid = false;
    // vkpt_physical_sky_record_cmd_buffer's last word, once the sky is recorded
    if (drawn && frame_sky_update)
        vkpt_physical_sky_set_needs_update(false);
    frame_sky_update = false;
    // a batch nothing baked: the bake starts again rather than leave a hole
    if (!drawn && frame_took_bake)
        vkpt_sky_exposure_restart();
    frame_took_bake = false;
}

/* R_RenderFrame_RTX's first test: while cpu_bench runs and while its result is up, the
   frame is the CPU path tracer's picture, drawn as a 2D pic, and the GPU renders no
   world. The player setup's model view still renders. */
bool d3d12_frontend_cpu_bench_draw(refdef_t *fd)
{
    if ((fd->rdflags & RDF_NOWORLDMODEL) || !vkpt_cpu_trace_active())
        return false;
    vkpt_refdef.fd = fd;
    vkpt_cpu_trace_draw();
    return true;
}

bool d3d12_frontend_prepare_frame(refdef_t *fd, int waterLevel, d3d12_world_frame_t *frame)
{
    // the player setup menu's model has no world under it, and from the main menu no
    // map is loaded at all: it needs none (vkpt_prepare_frame's rule)
    const bool render_world = (fd->rdflags & RDF_NOWORLDMODEL) == 0;
    if (!d3d12_world_ready() ||
        (render_world && (!d3d12_world_loaded() || !vkpt_refdef.bsp_mesh_world_loaded)))
        return false;

    // what vkpt's R_BeginFrame and vkpt_initialize_all would have set
    qvk.extent_unscaled.width = (uint32_t)d3d.width;
    qvk.extent_unscaled.height = (uint32_t)d3d.height;
    qvk.frame_counter = d3d.frame_counter;
    qvk.current_frame_index = d3d.frame_index;
    // NGX for the first frame that wants DLSS; if it cannot start, DLSS is off
    // until pt_dlss is changed (DLSSFeatureFailed)
    if (DLSSEnabled() && !ngx_params && !ngx_start_failed && !dlss_start()) {
        ngx_start_failed = true;
        DLSSFeatureFailed();
    }
    // DRS (vkpt main.c's drs_process, over this renderer's PROFILER_FRAME_TIME):
    // the render scale, photo mode's pin included. The images are made for the
    // largest scale it reaches (render_scale_capacity), so changing it - a DRS
    // step, photo mode in or out - remakes nothing (ensure_images compares them).
    fd->feedback.resolution_scale = vkpt_drs_update();
    vkpt_compute_extents();

    const vkpt_frame_prep_t *prep = &frame_prep;
    frame_prep_valid = false;
    if (!vkpt_prepare_frame(fd, waterLevel, &frame_prep))
        return false;
    frame_prep_valid = true;

    memset(frame, 0, sizeof(*frame));
    frame->no_world = !prep->render_world;

    // NVIDIA Flow, where R_RenderFrame_RTX runs it: Flow's simulation executes on the
    // queue now, ahead of this frame's list, and the textures set's Flow bindings are
    // this frame's; then what is left to record, and path_tracer.c's variants
    if (vkpt_flow_backend) {
        static cvar_t *pt_flow_split_pass, *pt_reflect_noflow;
        if (!pt_flow_split_pass) {
            pt_flow_split_pass = Cvar_Get("pt_flow_split_pass", "1", 0);    // path_tracer.c's
            pt_reflect_noflow = Cvar_Get("pt_reflect_noflow", "1", 0);
        }
        // the model view: the fire holds still and stays out of it (no march)
        vkpt_flow_update(fd, prep->render_world);
        vkpt_flow_dispatch_t fdisp;
        vkpt_flow_dispatches(&fdisp);
        const bool marching = vkpt_flow_marching();
        frame->flow.set = fdisp.set;
        frame->flow.occupancy_blocks = fdisp.occupancy_blocks;
        frame->flow.light_blocks = fdisp.light_blocks;
        memcpy(frame->flow.env_groups, fdisp.env_groups, sizeof(frame->flow.env_groups));
        frame->flow.split = marching && pt_flow_split_pass->integer != 0;
        frame->flow.reflect = marching || pt_reflect_noflow->integer == 0;
    }
    // the profiler's FLOW_MARCH row (draw_profiler asks path_tracer.c)
    vkpt_pt_set_flow_split_ran(frame->flow.split);

    // update_transparency's CPU half, as R_RenderFrame_RTX runs it after the
    // frame's preparation: the particles, beams and sprites into transparency.c's
    // frame, which d3d12_world_render copies into this slot's buffers. The CPU
    // half again (it does nothing once made): prepare writes into it.
    vkpt_transparency_cpu_init();
    vkpt_transparency_prepare(*vkpt_refdef.uniform_buffer.V, fd->particles, fd->num_particles, fd->entities,
                              fd->num_entities);
    vkpt_transparency_frame_t tf;
    vkpt_transparency_frame(&tf);
    d3d12_world_transparency_t *tr = &frame->tr;
    tr->data = tf.data;
    tr->upload_size = tf.upload_size;
    for (int i = 0; i < D3D12_TR_NUM_STREAMS; i++) {
        tr->offset[i] = tf.offset[i];
        tr->size[i] = tf.size[i];
    }
    tr->particle_num = tf.particle_num;
    tr->beam_num = tf.beam_num;
    tr->sprite_num = tf.sprite_num;
    tr->sprite_vertex_offset = tf.sprite_vertex_offset;

    // vkpt_blood_update's CPU half: the droplets into blood.c's shadow copy of
    // their section, and the regions d3d12_world_render copies out of it. Before
    // the plan, which builds the section's BLAS over what it wrote.
    vkpt_blood_frame_t blood;
    if (d3d12_world_take_blood_lost())
        vkpt_blood_invalidate();
    vkpt_blood_prepare(fd->blood_spheres, fd->num_blood_spheres, fd->blood_films, fd->num_blood_films,
                       prep->bsp, fd->vieworg, prep->blood_model_instance,
                       frame_prep.upload_info.blood_prim_offset, &blood);
    frame_prep.upload_info.blood_prim_count = blood.prim_count;
    frame->blood.prim_offset = blood.prim_offset;
    frame->blood.prim_count = blood.prim_count;
    frame->blood.zero_section = blood.zero_section;
    frame->blood.num_regions = blood.num_regions;
    frame->blood.regions = (const d3d12_blood_region_t *)blood.regions;
    frame->blood.prims = blood.prims;
    frame->blood.positions = blood.positions;
    frame->blood.capacity = blood.capacity;
    vkpt_blood_print_stats(d3d12_world_blood_copy_usec());  // pt_blood_stats (its copy is last frame's)

    // vkpt_pt_create_all_dynamic's BLASes, laid out in this frame slot's memory -
    // then added to the instance list after the static instances, as
    // vkpt_pt_create_toplevel adds them: the model sections, blood and
    // explosions of the instanced buffers, the particles, beams and sprites.
    const EntityUploadInfo *u = &prep->upload_info;
    const uint32_t first[D3D12_NUM_DYN_RUNS] = {
        [D3D12_DYN_OPAQUE]          = 0,
        [D3D12_DYN_TRANSPARENT]     = u->transparent_prim_offset,
        [D3D12_DYN_MASKED]          = u->masked_prim_offset,
        [D3D12_DYN_VIEWER_WEAPON]   = u->viewer_weapon_prim_offset,
        [D3D12_DYN_VIEWER_MODELS]   = u->viewer_model_prim_offset,
        [D3D12_DYN_BLOOD]           = u->blood_prim_offset,
        [D3D12_DYN_EXPLOSIONS]      = u->explosions_prim_offset,
    };
    const uint32_t count[D3D12_NUM_DYN_RUNS] = {
        [D3D12_DYN_OPAQUE]          = u->opaque_prim_count,
        [D3D12_DYN_TRANSPARENT]     = u->transparent_prim_count,
        [D3D12_DYN_MASKED]          = u->masked_prim_count,
        [D3D12_DYN_VIEWER_WEAPON]   = u->viewer_weapon_prim_count,
        [D3D12_DYN_VIEWER_MODELS]   = u->viewer_model_prim_count,
        [D3D12_DYN_BLOOD]           = u->blood_prim_count,
        [D3D12_DYN_EXPLOSIONS]      = u->explosions_prim_count,
    };
    uint64_t blas[VKPT_NUM_DYN];
    // pt_blood_blas_reuse (blood.c's BLAS REUSE): settled blood keeps this slot's BLAS
    const bool dyn_fast_build = cvar_pt_blas_fast_trace->integer == 0;
    const bool blood_reuse = !vkpt_blood_blas_needs_build(d3d.frame_index, u->blood_prim_offset,
                                                          u->blood_prim_count, dyn_fast_build);
    bool blood_built = false;
    d3d12_world_plan_dynamic(first, count, tr, !dyn_fast_build, blood_reuse, blas, &blood_built);
    if (blood_built)
        vkpt_blood_blas_built(d3d.frame_index, u->blood_prim_offset, u->blood_prim_count, dyn_fast_build);
    vkpt_pt_instance_list(&frame->first_dynamic_instance);
    frame->num_geometry_instances = vkpt_pt_append_dynamic_instances(blas, u, u->weapon_left_handed);
    frame->num_world_instances = vkpt_pt_num_world_instances();
    frame->ptlas = cvar_pt_ptlas->integer != 0;

    frame->ubo = &vkpt_refdef.uniform_buffer;
    frame->instance_buffer = &vkpt_refdef.uniform_instance_buffer;
    frame->instances = vkpt_pt_instance_list(&frame->num_instances);
    // vkpt_instance_geometry's arguments, the uploads' extents
    frame->num_model_instances = u->num_instances;
    frame->animate_world = prep->update_world_animations;
    frame->iqm_upload_size = vkpt_iqm_upload_size();
    frame->tlas_fast_trace = cvar_pt_tlas_fast_trace->integer != 0;
    frame->extents.screen_width = qvk.extent_screen_images.width;
    frame->extents.screen_height = qvk.extent_screen_images.height;
    frame->extents.unscaled_width = qvk.extent_unscaled.width;
    frame->extents.unscaled_height = qvk.extent_unscaled.height;
    frame->extents.taa_width = qvk.extent_taa_images.width;
    frame->extents.taa_height = qvk.extent_taa_images.height;
    frame->extents.profile = vkpt_screen_image_profile();
    frame->render_width = qvk.extent_render.width;
    frame->render_height = qvk.extent_render.height;
    frame->field_width = vkpt_pt_field_width();
    // TILE COMPACTION: the lists' header for the screen images' layout, or none this view
    {
        static uint32_t tile_header[PT_TILE_HDR_WORDS];
        vkpt_tile_layout_t layout;
        vkpt_pt_tile_layout(qvk.extent_screen_images.width, qvk.extent_screen_images.height, &layout);
        frame->tile_header = vkpt_pt_tile_header(&layout, D3D12_TILE_MAX_GROUPS, tile_header) ? tile_header : NULL;
    }
    frame->slice_width = qvk.gpu_slice_width;
    frame->taa_output_width = qvk.extent_taa_output.width;
    frame->taa_output_height = qvk.extent_taa_output.height;
    frame->num_light_regions = vkpt_light_upload_regions(frame->light_offsets, frame->light_sizes);
    frame->enable_denoiser = prep->ref_mode.enable_denoiser;
    frame->num_bounce_rays = prep->ref_mode.num_bounce_rays;
    frame->caustics = cvar_pt_caustics->value != 0;
    frame->sharc_active = prep->sharc_active;
    frame->sharc_clear = prep->sharc_clear;
    // DLSSApply (d3d12_frontend_dlss_record), and whether the interleave pass
    // writes TAA_OUTPUT itself (the UBO is this frame's)
    frame->dlss = DLSSEnabled() && ngx_params;
    // photo mode bypasses DLSS (vkpt's accumulation_bypasses_dlss): the accumulation
    // is the image. NGX saw none of the pause, so the first frame after it starts its
    // history again, as vkpt asks every bypassed frame.
    frame->dlss_bypass = frame->dlss && prep->ref_mode.enable_accumulation && vkpt_fg_bypassed();
    if (frame->dlss_bypass)
        vkpt_dlss_request_history_reset();
    // DLSS 5 Neural Rendering (DLSSNR.c), as R_EndFrame_RTX runs it: on DLSS's arm, not
    // in photo mode. The feature is made in the passes' callback, which may say no.
    frame->dlss5 = frame->dlss && !frame->dlss_bypass && DLSS5Enabled();
    if (frame->dlss5) {
        typedef char dlss5_push_check[sizeof(dlss5_convert_push_t) == sizeof(frame->dlss5_push[0]) ? 1 : -1];
        dlss5_convert_push_t push;
        for (int mode = 0; mode < 2; mode++) {
            DLSS5ConvertPush(mode, qvk.extent_unscaled.width, qvk.extent_unscaled.height, d3d.hdr ? qtrue : qfalse,
                             &push);
            memcpy(frame->dlss5_push[mode], &push, sizeof(push));
        }
    }
    frame->taa_folded = vkpt_taa_folded();
    // bloom.c's pass: over TAA_OUTPUT, or with DLSS over DLSS_OUTPUT at the output size
    frame->bloom = vkpt_bloom_will_run();
    {
        // bloom.c's view of a stage of the bloom (bloom_debug_show_image)
        static cvar_t *bloom_debug;
        if (!bloom_debug)
            bloom_debug = Cvar_Get("bloom_debug", "0", 0);
        frame->bloom_debug = bloom_debug->integer >= 1 && bloom_debug->integer <= 3 ? bloom_debug->integer : 0;
    }
    vkpt_bloom_blur_params(DLSSEnabled() ? qvk.extent_unscaled.height : qvk.extent_taa_output.height,
                           &frame->bloom_argument_scale, &frame->bloom_normalization_scale,
                           &frame->bloom_num_samples);

    // motion_blur.c's and chromatic_aberration.c's passes, the same shaders with the
    // same push constants. They pick their image by DLSSEnabled(), so only while that
    // agrees with the frame's DLSS arm (not the frame NGX is still starting on).
    // the sizes passes.h carries them in (no C11 here: a negative array fails to compile)
    typedef char motion_blur_push_check[sizeof(vkpt_motion_blur_push_t) == sizeof(frame->motion_blur_push) ? 1 : -1];
    typedef char ca_push_check[sizeof(vkpt_chromatic_aberration_push_t) == sizeof(frame->chromatic_aberration_push) ? 1 : -1];
    typedef char blit_push_check[sizeof(vkpt_final_blit_push_t) == sizeof(frame->blit_effects_push) ? 1 : -1];
    frame->motion_blur = false;
    frame->motion_blur_to_bloom = false;
    frame->chromatic_aberration = false;
    if (frame->dlss == DLSSEnabled()) {
        vkpt_motion_blur_push_t blur;
        if (vkpt_motion_blur_is_enabled() && vkpt_motion_blur_params(&blur)) {
            frame->motion_blur = true;
            frame->motion_blur_to_bloom = vkpt_motion_blur_bloom_takes_copy();
            memcpy(frame->motion_blur_push, &blur, sizeof(blur));
        }
        vkpt_chromatic_aberration_push_t ca;
        if (vkpt_chromatic_aberration_is_enabled() && vkpt_chromatic_aberration_params(&ca)) {
            frame->chromatic_aberration = true;
            memcpy(frame->chromatic_aberration_push, &ca, sizeof(ca));
        }
    }
    // draw.c's film grain and CRT, at the window's size (world.c's blit)
    frame->blit_effects = vkpt_final_blit_effects_enabled();
    if (frame->blit_effects) {
        vkpt_final_blit_push_t fx;
        vkpt_final_blit_effects_params(d3d.width, d3d.height, &fx);
        memcpy(frame->blit_effects_push, &fx, sizeof(fx));
    }
    frame->frame_time = prep->frame_time <= 0.f ? prep->frame_wallclock_time : prep->frame_time;

    // R_RenderFrame_RTX records the sky in every frame with a world, when
    // vkpt_physical_sky_update_ubo (in vkpt_prepare_frame) or a cvar asked; the
    // request stands through the model view's frames (ENVIRONMENT_NONE)
    frame->sky_update = frame_sky_update = prep->render_world && vkpt_physical_sky_needs_update();
    frame->sky_space = vkpt_physical_sky_space_enabled();
    memcpy(frame->sky_viewproj, terrain_shadowmap_viewproj, sizeof(frame->sky_viewproj));

    // the fog: R_RenderFrame_RTX's switches, vkpt_shadow_map_render's arguments and
    // shadow_map.c's instances, and a batch of the sky exposure bake for the grid
    frame->god_rays = prep->god_rays_enabled && d3d12_world_fog_ready();
    frame->god_rays_march = !prep->god_rays_march_discarded;
    frame->fog_accum_march = prep->fog_accum_march;
    frame->froxel = frame->god_rays && prep->run_froxel;
    frame->froxel_restir = vkpt_froxel_restir_enabled();
    memcpy(frame->shadowmap_viewproj, prep->shadowmap_view_proj, sizeof(frame->shadowmap_viewproj));
    if (frame->god_rays) {
        const bsp_mesh_t *wm = &vkpt_refdef.bsp_mesh_world;
        if (wm->geom_opaque.num_geometries) {
            frame->smap_opaque_first = wm->geom_opaque.prim_offsets[0] * 3;
            frame->smap_opaque_count = wm->geom_opaque.prim_counts[0] * 3;
        }
        if (wm->geom_transparent.num_geometries) {
            frame->smap_water_first = wm->geom_transparent.prim_offsets[0] * 3;
            frame->smap_water_count = wm->geom_transparent.prim_counts[0] * 3;
        }
        frame->smap_dynamic_count = u->opaque_prim_count * 3;

        const vkpt_shadow_map_instance_t *si;
        const uint32_t n = min(vkpt_shadow_map_instances(&si), (uint32_t)MAX_MODEL_INSTANCES);
        for (uint32_t i = 0; i < n; i++) {
            d3d12_shadow_instance_t *o = &smap_instances[i];
            mult_matrix_matrix(o->mvp, prep->shadowmap_view_proj, si[i].model_matrix);
            o->model = si[i].buffer_idx == VERTEX_BUFFER_WORLD ? -1 : (int)si[i].buffer_idx - VERTEX_BUFFER_FIRST_MODEL;
            o->positions_offset = si[i].vertex_offset;
            o->prim_count = si[i].prim_count;
        }
        frame->smap_instances = smap_instances;
        frame->smap_num_instances = n;
    }
    if (frame->froxel)
        vkpt_sky_exposure_next_batch(&frame->sky_exposure_first, &frame->sky_exposure_count);
    frame_took_bake = frame->sky_exposure_count != 0;
    return true;
}

const void *d3d12_frontend_stretch_pics(int *count, size_t *record_size, int *max_count)
{
    return vkpt_draw_stretch_pic_queue(count, record_size, max_count);
}

void d3d12_frontend_clear_stretch_pics(void)
{
    vkpt_draw_clear_stretch_pics();
}

void d3d12_frontend_ui_hdr_params(float *ui_hdr_nits, float *saturation_scale, float *video_gamma)
{
    vkpt_draw_ui_hdr_params(ui_hdr_nits, saturation_scale, video_gamma);
}

// VKPT_SAMPLER_* and D3D12_GLOBAL_SAMPLER_* are the same list (fails to compile if not)
typedef char sampler_lists_match[((int)VKPT_NUM_SAMPLER_KINDS == (int)D3D12_NUM_GLOBAL_SAMPLERS &&
                                  (int)VKPT_SAMPLER_NEAREST == (int)D3D12_GLOBAL_SAMPLER_NEAREST &&
                                  (int)VKPT_SAMPLER_NEAREST_MIPMAP_ANISO == (int)D3D12_GLOBAL_SAMPLER_NEAREST_MIPMAP_ANISO &&
                                  (int)VKPT_SAMPLER_LINEAR_CLAMP == (int)D3D12_GLOBAL_SAMPLER_LINEAR_CLAMP) ? 1 : -1];

int d3d12_frontend_sampler_kind(const image_t *image, int pt_nearest, int bilerp_chars, int bilerp_pics)
{
    return vkpt_image_sampler_kind(image, pt_nearest, bilerp_chars, bilerp_pics);
}

void d3d12_frontend_register_functions(void)
{
    R_ClearColor = R_ClearColor_RTX;
    R_SetAlpha = R_SetAlpha_RTX;
    R_SetAlphaScale = R_SetAlphaScale_RTX;
    R_SetColor = R_SetColor_RTX;
    R_SetClipRect = R_SetClipRect_RTX;
    R_SetScale = R_SetScale_RTX;
    R_DrawChar = R_DrawChar_RTX;
    R_DrawString = R_DrawString_RTX;
    R_DrawPic = R_DrawPic_RTX;
    R_DrawStretchPic = R_DrawStretchPic_RTX;
    R_DrawRotatedPic = R_DrawRotatedPic_RTX;
    R_TileClear = R_TileClear_RTX;
    R_DrawFill8 = R_DrawFill8_RTX;
    R_DrawFill32 = R_DrawFill32_RTX;
    // the faces, the rotation, the map's skybox scope; the cube through
    // d3d12_backend's upload_envmap
    R_SetSky = R_SetSky_RTX;
    // photo mode's free camera (freecam.c): the keys it moves by, the depth of
    // field's wheel, and the mouse buttons it keeps from the game. Its per-frame
    // half, vkpt_freecam_update, already ran in the shared vkpt_prepare_frame
    R_InterceptKey = R_InterceptKey_RTX;

    MOD_LoadMD2 = MOD_LoadMD2_RTX;
#if USE_MD3
    MOD_LoadMD3 = MOD_LoadMD3_RTX;
#endif
    MOD_LoadIQM = MOD_LoadIQM_RTX;
    MOD_LoadMD5 = MOD_LoadMD5_RTX;
    MOD_Reference = MOD_Reference_RTX;
    MOD_TraceMesh = MOD_TraceMesh_RTX;
    // R_FlowEmit: explosions, rockets, the BFG and the railgun into Flow (nothing
    // until flow.c has a grid)
    vkpt_flow_register();
}
