/*
NGX THROUGH D3D12: DLSS Super Resolution and Ray Reconstruction, as vkpt/DLSS.c runs
them through NGX's Vulkan entry points. Which feature to make and what to hand it are
DLSS.c's (DLSSDescribeFeature, DLSSDescribeEvaluation, called from frontend.c); this is
only the API's half - the instance, the feature, the evaluation. It includes no engine
header, so tests/d3d12_world_test.c compiles it in and runs NGX for real.

RESOURCE STATES. NGX wants its inputs readable by a compute shader and its output in
UNORDERED_ACCESS at the evaluation (programming guide 3.4), and puts them back so if it
moves them. The G-buffer images are simultaneous-access textures (world.c), which
promote from COMMON to whatever an access needs, so they are handed over as they are.

COMMAND LIST STATE. An evaluation (and a creation) sets NGX's own descriptor heaps,
root signature and pipeline on the list (guide 5.4): the caller sets its heaps again
after it, and binds its root signature afresh (passes.c, after the upscale callback).
*/

#ifndef REFRESH_D3D12_NGX_H
#define REFRESH_D3D12_NGX_H

#include <stdbool.h>
#include <stdint.h>
#include <d3d12.h>

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd.h>

#include "../vkpt/dlssg_desc.h"

#ifdef __cplusplus
extern "C" {    // ngx_dlssg.cpp defines the frame generation half
#endif

/* NVSDK_NGX_D3D12_Init_with_ProjectID (a custom engine) and the capability block, with
   FreeMemOnReleaseFeature set as TryInit sets it. `log` may be NULL. On failure nothing
   is left up and *params is NULL. */
NVSDK_NGX_Result d3d12_ngx_init(ID3D12Device *device, const char *project_id, const char *engine_version,
                                const wchar_t *dll_path, const wchar_t *data_path, NVSDK_NGX_AppLogCallback log,
                                NVSDK_NGX_Parameter **params);
// the block and the device's instance; the GPU idle, every feature released
void d3d12_ngx_shutdown(ID3D12Device *device, NVSDK_NGX_Parameter *params);

/* The feature, recorded into `cmd` (NGX_D3D12_CREATE_DLSS_EXT or _DLSSD_EXT) on node 1:
   Super Resolution from `sr`, or with `rr` Ray Reconstruction from `rr_create`. */
NVSDK_NGX_Result d3d12_ngx_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Parameter *params, bool rr,
                                  NVSDK_NGX_DLSS_Create_Params *sr, NVSDK_NGX_DLSSD_Create_Params *rr_create,
                                  NVSDK_NGX_Handle **feature);
// NVSDK_NGX_D3D12_ReleaseFeature; the GPU must be done with it
NVSDK_NGX_Result d3d12_ngx_release(NVSDK_NGX_Handle *feature);

/* One evaluation, as DLSSApply makes it: the render's top-left width x height of each
   input (motion vectors in UV, scaled by that size to pixels), the output whole. NULL
   leaves an optional guide out. */
typedef struct {
    bool            rr;                 // the feature is Ray Reconstruction
    ID3D12Resource  *color, *output, *depth, *motion;
    // Ray Reconstruction's guides
    ID3D12Resource  *diffuse_albedo, *specular_albedo, *normals, *roughness, *specular_hit_distance;
    ID3D12Resource  *reflection_motion, *diffuse_hit_distance, *reflected_albedo, *before_transparency;
    ID3D12Resource  *responsivity_mask;     // "DLSSD.ResponsivityMask" (pt_dlss_glass_responsive), NULL = off
    uint32_t        width, height;
    float           jitter_x, jitter_y;  // render pixels
    bool            reset;
    bool            matrices;           // world_to_view and view_to_clip go over (Ray Reconstruction)
    float           world_to_view[16], view_to_clip[16];
} d3d12_ngx_eval_t;

NVSDK_NGX_Result d3d12_ngx_evaluate(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *feature,
                                    NVSDK_NGX_Parameter *params, d3d12_ngx_eval_t *e);

/* DLSS 5 NEURAL RENDERING (NGX feature 18; vkpt/DLSSNR.c says what it is told). A
   parameter block of its own from the instance (DLSSNR.c keeps it), the feature recorded
   into `cmd`, and one evaluation: every parameter but the resources is on `params`
   already (DLSS5DescribeEvaluation); the resources go over as D3D12 ones, as NGX's
   helpers hand theirs (SetD3d12Resource). `control` is DLSSNR.ControlMask, NULL for none
   (pt_dlss5_volumetric 0) - set either way, since the block outlives the frame. */
NVSDK_NGX_Result d3d12_ngx_new_params(NVSDK_NGX_Parameter **params);
void d3d12_ngx_destroy_params(NVSDK_NGX_Parameter *params);
NVSDK_NGX_Result d3d12_ngx_dlss5_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Parameter *params,
                                        NVSDK_NGX_Handle **feature);
NVSDK_NGX_Result d3d12_ngx_dlss5_evaluate(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *feature,
                                          NVSDK_NGX_Parameter *params, ID3D12Resource *color, ID3D12Resource *depth,
                                          ID3D12Resource *motion, ID3D12Resource *output,
                                          ID3D12Resource *control);

/* FRAME GENERATION (ngx_dlssg.cpp), as vkpt/DLSSG.cpp makes it. The feature, recorded
   into `cmd` on node 1: `width` x `height` the output (the backbuffer handed over, in
   `backbuffer_format`), the render size the depth and motion vectors'. Its
   `generated_frames` per real frame is a create-time property: a new count needs a new
   feature (ValidateDLSSGFeature's rule). Released with d3d12_ngx_release. */
NVSDK_NGX_Result d3d12_ngx_dlssg_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Parameter *params,
                                        uint32_t width, uint32_t height,
                                        uint32_t render_width, uint32_t render_height,
                                        DXGI_FORMAT backbuffer_format, uint32_t generated_frames,
                                        NVSDK_NGX_Handle **feature);

/* One generated frame: `desc` is DLSSGDescribeEvaluation's, with multiFrameIndex this
   frame's (1..multiFrameCount). `hudless` NULL leaves the runtime to find the UI. */
typedef struct {
    ID3D12Resource          *backbuffer;    // the real frame, display-encoded (output size)
    ID3D12Resource          *depth, *motion;    // render size
    ID3D12Resource          *hudless;       // the backbuffer again: there is no HUD in it
    ID3D12Resource          *output;        // the generated frame (output size)
    const DLSSGEvalDesc     *desc;
    uint64_t                frame_id;       // DLSSG.BackbufferFrameID
} d3d12_ngx_dlssg_eval_t;

NVSDK_NGX_Result d3d12_ngx_dlssg_evaluate(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *feature,
                                          NVSDK_NGX_Parameter *params, const d3d12_ngx_dlssg_eval_t *e);

#ifdef __cplusplus
}
#endif

#endif // REFRESH_D3D12_NGX_H
