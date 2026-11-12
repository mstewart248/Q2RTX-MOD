/*
NGX through D3D12 - see ngx.h. Mirrors vkpt/DLSS.c's TryInit, ValidateDLSSFeature's
creation and DLSSApply's evaluation, with NGX's D3D12 helpers for its Vulkan ones.
*/

#include "ngx.h"

NVSDK_NGX_Result d3d12_ngx_init(ID3D12Device *device, const char *project_id, const char *engine_version,
                                const wchar_t *dll_path, const wchar_t *data_path, NVSDK_NGX_AppLogCallback log,
                                NVSDK_NGX_Parameter **params)
{
    *params = NULL;

    NVSDK_NGX_FeatureCommonInfo common = { 0 };
    common.PathListInfo.Path = &dll_path;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.LoggingCallback = log;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

    NVSDK_NGX_Result res = NVSDK_NGX_D3D12_Init_with_ProjectID(project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                               engine_version, data_path, device, &common,
                                                               NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(res))
        return res;

    res = NVSDK_NGX_D3D12_GetCapabilityParameters(params);
    if (NVSDK_NGX_FAILED(res)) {
        *params = NULL;
        NVSDK_NGX_D3D12_Shutdown1(device);
        return res;
    }

    // DLSS.c's TryInit says why: without it NGX keeps a released feature's VRAM, and
    // every resize makes a new one
    NVSDK_NGX_Parameter_SetUI(*params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    return res;
}

void d3d12_ngx_shutdown(ID3D12Device *device, NVSDK_NGX_Parameter *params)
{
    if (params)
        NVSDK_NGX_D3D12_DestroyParameters(params);
    NVSDK_NGX_D3D12_Shutdown1(device);
}

NVSDK_NGX_Result d3d12_ngx_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Parameter *params, bool rr,
                                  NVSDK_NGX_DLSS_Create_Params *sr, NVSDK_NGX_DLSSD_Create_Params *rr_create,
                                  NVSDK_NGX_Handle **feature)
{
    // one GPU: created on node 1, visible to it
    *feature = NULL;
    const NVSDK_NGX_Result res = rr ? NGX_D3D12_CREATE_DLSSD_EXT(cmd, 1, 1, feature, params, rr_create)
                                    : NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, feature, params, sr);
    if (NVSDK_NGX_FAILED(res))
        *feature = NULL;
    return res;
}

NVSDK_NGX_Result d3d12_ngx_release(NVSDK_NGX_Handle *feature)
{
    return NVSDK_NGX_D3D12_ReleaseFeature(feature);
}

NVSDK_NGX_Result d3d12_ngx_evaluate(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *feature,
                                    NVSDK_NGX_Parameter *params, d3d12_ngx_eval_t *e)
{
    const NVSDK_NGX_Dimensions size = { e->width, e->height };
    const NVSDK_NGX_Coordinates origin = { 0, 0 };

    if (!e->rr) {
        NVSDK_NGX_D3D12_DLSS_Eval_Params p = { 0 };
        p.Feature.pInColor = e->color;
        p.Feature.pInOutput = e->output;
        p.pInDepth = e->depth;
        p.pInMotionVectors = e->motion;
        p.InJitterOffsetX = e->jitter_x;
        p.InJitterOffsetY = e->jitter_y;
        p.InRenderSubrectDimensions = size;
        p.InReset = e->reset ? 1 : 0;
        p.InMVScaleX = (float)e->width;
        p.InMVScaleY = (float)e->height;
        p.InColorSubrectBase = origin;
        p.InDepthSubrectBase = origin;
        p.InMVSubrectBase = origin;
        return NGX_D3D12_EVALUATE_DLSS_EXT(cmd, feature, params, &p);
    }

    NVSDK_NGX_D3D12_DLSSD_Eval_Params p = { 0 };
    p.pInColor = e->color;
    p.pInOutput = e->output;
    p.pInDepth = e->depth;
    p.pInMotionVectors = e->motion;
    p.InJitterOffsetX = e->jitter_x;
    p.InJitterOffsetY = e->jitter_y;
    p.InRenderSubrectDimensions = size;
    p.InReset = e->reset ? 1 : 0;
    p.InMVScaleX = (float)e->width;
    p.InMVScaleY = (float)e->height;
    p.InColorSubrectBase = origin;
    p.InDepthSubrectBase = origin;
    p.InMVSubrectBase = origin;
    p.pInMotionVectorsReflections = e->reflection_motion;
    p.pInWorldToViewMatrix = e->matrices ? e->world_to_view : NULL;
    p.pInViewToClipMatrix = e->matrices ? e->view_to_clip : NULL;
    p.pInNormals = e->normals;
    p.pInDiffuseHitDistance = e->diffuse_hit_distance;
    p.pInSpecularHitDistance = e->specular_hit_distance;
    p.pInSpecularAlbedo = e->specular_albedo;
    p.pInDiffuseAlbedo = e->diffuse_albedo;
    p.pInRoughness = e->roughness;
    p.pInReflectedAlbedo = e->reflected_albedo;
    p.InReflectedAlbedoSubrectBase = origin;
    p.pInColorBeforeTransparency = e->before_transparency;
    p.InColorBeforeTransparencySubrectBase = origin;
    // RR2's responsivity mask is newer than these 310.6 helpers: set it by name, and to
    // NULL when off - the parameter block outlives the frame (DLSSDescribeEvaluation)
    NVSDK_NGX_Parameter_SetD3d12Resource(params, "DLSSD.ResponsivityMask", e->responsivity_mask);
    NVSDK_NGX_Parameter_SetUI(params, "DLSSD.ResponsivityMask.Subrect.Base.X", origin.X);
    NVSDK_NGX_Parameter_SetUI(params, "DLSSD.ResponsivityMask.Subrect.Base.Y", origin.Y);
    return NGX_D3D12_EVALUATE_DLSSD_EXT(cmd, feature, params, &p);
}

/* DLSS 5: not in the SDK's headers (310.6 stops at NVSDK_NGX_Feature_Reserved17); the
   static library passes the id to the driver's core, which routes 18 to
   nvngx_dlssnr.dll (DLSSNR.c's DLSS5_NGX_FEATURE). Every control parameter, the jitter
   included, is DLSSNR.c's (DLSS5DescribeEvaluation), shared with Vulkan.

   The resource keys are the ones NVIDIA's DLSS 5 header defines (nvsdk_ngx_defs_dlssnr.h,
   310.9.16) and its Vulkan helper sets with SetVoidPointer. That SDK package ships no D3D12
   helper (nvsdk_ngx_helpers_dlssnr.h includes a _d3d.h it does not contain), so these go
   over as D3D12 resources on the same keys, the way every other NGX feature's D3D12 helper
   does it. Not yet confirmed on hardware. */
#define D3D12_NGX_FEATURE_DLSS5 ((NVSDK_NGX_Feature)18)

NVSDK_NGX_Result d3d12_ngx_new_params(NVSDK_NGX_Parameter **params)
{
    *params = NULL;
    const NVSDK_NGX_Result res = NVSDK_NGX_D3D12_GetCapabilityParameters(params);
    if (NVSDK_NGX_FAILED(res))
        *params = NULL;
    return res;
}

void d3d12_ngx_destroy_params(NVSDK_NGX_Parameter *params)
{
    NVSDK_NGX_D3D12_DestroyParameters(params);
}

NVSDK_NGX_Result d3d12_ngx_dlss5_create(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Parameter *params,
                                        NVSDK_NGX_Handle **feature)
{
    *feature = NULL;
    const NVSDK_NGX_Result res = NVSDK_NGX_D3D12_CreateFeature(cmd, D3D12_NGX_FEATURE_DLSS5, params, feature);
    if (NVSDK_NGX_FAILED(res))
        *feature = NULL;
    return res;
}

NVSDK_NGX_Result d3d12_ngx_dlss5_evaluate(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *feature,
                                          NVSDK_NGX_Parameter *params, ID3D12Resource *color, ID3D12Resource *depth,
                                          ID3D12Resource *motion, ID3D12Resource *output,
                                          ID3D12Resource *control)
{
    NVSDK_NGX_Parameter_SetD3d12Resource(params, "DLSSNR.Color", color);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, "DLSSNR.Depth", depth);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, "DLSSNR.MVec", motion);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, "DLSSNR.Output", output);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, "DLSSNR.ControlMask", control);
    return NVSDK_NGX_D3D12_EvaluateFeature_C(cmd, feature, params, NULL);
}
