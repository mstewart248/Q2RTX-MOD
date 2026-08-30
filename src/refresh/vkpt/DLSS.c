// Copyright (c) 2023 Matt Stewart (Stolen and Bastardized) from Sultim Tsyrendashiev
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "DLSS.h"
#include "DLSSG.h"
#include "reflex.h"

DLSS dlssObj;
cvar_t* cvar_pt_dlss = NULL;
cvar_t* cvar_pt_dlss_dldn = NULL;
cvar_t* cvar_pt_dlss_split_fields = NULL;
cvar_t* cvar_pt_dlss_bypass_denoiser = NULL;
cvar_t* cvar_pt_dlss_field_res = NULL;
cvar_t* cvar_pt_dlss_diff_hitdist = NULL;
cvar_t* cvar_pt_dlss_reflected_albedo = NULL;
qboolean recreateSwapChain = qfalse;
qboolean dlssModeChanged = qfalse;
extern cvar_t* scr_viewsize;
extern cvar_t* vid_rtx;
int oldCvarValue;

void InitDLSSCvars() 
{
    cvar_pt_dlss = Cvar_Get("pt_dlss", "0", CVAR_ARCHIVE);
    cvar_pt_dlss_dldn = Cvar_Get("pt_dlss_dldn", "0", CVAR_ARCHIVE);

    // Full-resolution reflection/refraction fields instead of checkerboarding them.
    //   0 - off, classic Q2RTX checkerboard
    //   1 - on whenever DLSS is enabled (Super Resolution or Ray Reconstruction)
    //   2 - always on, even without DLSS
    // Checkerboard rendering is explicitly listed as a practice to avoid in the
    // DLSS-RR integration guide (3.5), and it also feeds DLSS-SR inconsistent motion
    // vectors on glass and water. Costs a second lighting pass over the frame.
    cvar_pt_dlss_split_fields = Cvar_Get("pt_dlss_split_fields", "1", CVAR_ARCHIVE);

    // Whether DLSS Ray Reconstruction replaces A-SVGF entirely (1) or runs on top of it
    // (0). 1 is what the RR integration guide asks for - RR wants the raw noisy signal
    // and assumes independent samples, which A-SVGF's temporal accumulation violates.
    // 0 restores the pre-existing behaviour of stacking both denoisers, which is less
    // correct but much smoother if RR alone is not converging.
    cvar_pt_dlss_bypass_denoiser = Cvar_Get("pt_dlss_bypass_denoiser", "1", CVAR_ARCHIVE);

    // Hand DLSS-RR the diffuse hit distance guide. IMG_PT_RAYLENGTH_DIFFUSE had no live
    // writer at all until 2026-08-21 and the eval param was commented out, so RR was
    // sizing its diffuse spatial filter with no signal. 0 restores that for an A/B.
    // Registered here rather than lazily at eval time so it exists from startup and
    // shows up in cvarlist.
    cvar_pt_dlss_diff_hitdist = Cvar_Get("pt_dlss_diff_hitdist", "1", CVAR_ARCHIVE);

    // Reflected albedo guide - the base colour of the surface seen IN the reflection, so
    // RR can demodulate before filtering instead of blurring the reflected texture.
    // IMG_PT_REFLECTED_ALBEDO was written and cleared all along but never handed over.
    // Default 0: not judged by eye yet.
    cvar_pt_dlss_reflected_albedo = Cvar_Get("pt_dlss_reflected_albedo", "0", CVAR_ARCHIVE);

    // Resolution of the reflection/refraction layers while split fields are active.
    //   1 - both layers at full internal render resolution
    //   2 - both layers at half resolution: they are traced on alternating rows and the
    //       combine pass fills the untraced rows from their neighbour. Same ray count as
    //       the classic checkerboard, but coherent rather than interleaved. Only
    //       reflect/refract materials are affected - opaque geometry stays full res.
    // No effect when pt_dlss_split_fields resolves to off. Safe to change live - the field
    // layout and every image size stay the same, so no renderer reinitialization.
    cvar_pt_dlss_field_res = Cvar_Get("pt_dlss_field_res", "1", CVAR_ARCHIVE);

    oldCvarValue = cvar_pt_dlss->integer;
    cvar_pt_dlss->changed = viewsize_changed;
    cvar_pt_dlss_dldn->changed = DlssModeChanged;
    cvar_pt_dlss_split_fields->changed = DlssSplitFieldsChanged;
    cvar_pt_dlss_field_res->changed = DlssFieldResChanged;
    viewsize_changed(cvar_pt_dlss);
}

// True when the path tracer should trace two full-resolution layers (field 0 =
// reflection, field 1 = refraction) rather than two checkerboard halves.
// Multi-GPU is excluded: there the two fields are how work is split across devices.
qboolean DLSSSplitFieldsEnabled() {
    if (cvar_pt_dlss_split_fields == NULL)
        return qfalse;

    if (qvk.device_count > 1)
        return qfalse;

    switch (cvar_pt_dlss_split_fields->integer) {
        case 1:  return DLSSEnabled();
        case 2:  return qtrue;
        default: return qfalse;
    }
}

// True when the reflection and refraction layers are traced at half vertical
// resolution - alternating rows, filled in from the neighbouring row by the combine
// pass. Applies only to reflect/refract materials; opaque geometry is always full
// resolution.
qboolean DLSSFieldHalfRes() {
    if (!DLSSSplitFieldsEnabled())
        return qfalse;

    return (cvar_pt_dlss_field_res != NULL && cvar_pt_dlss_field_res->integer == 2)
        ? qtrue : qfalse;
}

// Report what the cvar actually resolved to. It silently does nothing while
// pt_dlss_split_fields resolves to off, which is easy to mistake for the mode being
// broken.
void DlssFieldResChanged(cvar_t* self) {
    if (!DLSSSplitFieldsEnabled()) {
        Com_Printf("pt_dlss_field_res %d: no effect - split fields are off "
            "(pt_dlss_split_fields %d, pt_dlss %d)\n", self->integer,
            cvar_pt_dlss_split_fields ? cvar_pt_dlss_split_fields->integer : 0,
            cvar_pt_dlss ? cvar_pt_dlss->integer : 0);
        return;
    }

    Com_Printf("pt_dlss_field_res %d: reflection/refraction at %s render resolution\n",
        self->integer, (self->integer == 2) ? "half" : "full");
}

// True when DLSS Ray Reconstruction is the active denoiser, i.e. A-SVGF must be
// bypassed and the RR guide buffers are what matters.
qboolean DLSSRayReconstructionActive() {
    return (DLSSEnabled() && DLSSModeDenoise() == 1) ? qtrue : qfalse;
}

// True when RR should be the *only* denoiser, i.e. A-SVGF must be skipped.
qboolean DLSSBypassDenoiser() {
    if (!DLSSRayReconstructionActive())
        return qfalse;
    return (cvar_pt_dlss_bypass_denoiser != NULL && cvar_pt_dlss_bypass_denoiser->integer != 0)
        ? qtrue : qfalse;
}

void DlssSplitFieldsChanged(cvar_t* self) {
    // Changing the field layout changes the size of every path-tracer screen image,
    // so the renderer has to be reinitialized.
    recreateSwapChain = qtrue;
    Cvar_SetByVar(vid_rtx, "0", FROM_MENU);
    Cvar_SetByVar(vid_rtx, "1", FROM_MENU);
}

qboolean DLSSCreated() {
    return dlssObj.created;
}

qboolean DLSSEnabled() {
    if (cvar_pt_dlss->integer != 0) {
        return qtrue;
    }
    else {
        return qfalse;
    }
}

int DLSSMode() {
    return cvar_pt_dlss->integer;
}

int DLSSModeDenoise() {
    return cvar_pt_dlss_dldn->integer;
}

float GetDLSSResolutionScale() {
    switch (cvar_pt_dlss->integer) {
        case -1:
            return .25f;
        case 1:
            return .5f;
        case 2:
            return .59f;
        case 3:
            return .66f;
        case 4:
        case 5:
            return 1.0f;
        default:
            return 1.0;
    }
}

float GetDLSSMultResolutionScale() {
    switch (cvar_pt_dlss->integer) {
    case -1:
        return (4 * .25f);
    case 1:
        return (4 * .5f);
    case 2:
        return (4 * .59f);
    case 3:
        return (4 * .66f);
    case 4: 
    case 5:
        return 1;
    default:
        return 1.0;
    }
}

// Spike: does this driver expose DLSS Frame Generation to us?
// Reads the SAME capability params as the SR/RR checks, already populated by the
// NVSDK_NGX_VULKAN_Init_with_ProjectID call in TryInit(). Purely diagnostic:
// creates no feature and changes no behaviour.
//
// NOTE: only nvsdk_ngx_defs_dlssg.h is included (see DLSS.h). The DLSS-G
// *helper* headers (nvsdk_ngx_params_dlssg.h / nvsdk_ngx_helpers_dlssg_vk.h)
// use C++ default member initializers and will not compile as C.
static void ReportOneFrameGenFeature(const char* label,
                                     const char* keyAvailable,
                                     const char* keyInitResult,
                                     const char* keyNeedsDriver,
                                     const char* keyMajor,
                                     const char* keyMinor)
{
    float available = 0.0f;
    NVSDK_NGX_Result res = NVSDK_NGX_Parameter_GetF(dlssObj.pParams, keyAvailable, &available);

    if (NVSDK_NGX_FAILED(res)) {
        Com_Printf("DLSS-G: %s -> query failed, NGX does not know this key (res=0x%08x)\n",
                   label, (unsigned)res);
        return;
    }

    Com_Printf("DLSS-G: %s available = %s\n", label, available ? "YES" : "no");

    float needsUpdatedDriver = 0.0f;
    float minMajor = 0.0f;
    float minMinor = 0.0f;

    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetF(dlssObj.pParams, keyNeedsDriver, &needsUpdatedDriver))
        && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetF(dlssObj.pParams, keyMajor, &minMajor))
        && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetF(dlssObj.pParams, keyMinor, &minMinor)))
    {
        Com_Printf("DLSS-G: %s min driver = %d.%d%s\n", label,
                   (int)minMajor, (int)minMinor,
                   needsUpdatedDriver ? "  (DRIVER TOO OLD)" : "");
    }
    else {
        Com_Printf("DLSS-G: %s min driver not reported\n", label);
    }

    if (!available) {
        NVSDK_NGX_Result featureInitResult = NVSDK_NGX_Result_Fail;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(dlssObj.pParams, keyInitResult, (int*)&featureInitResult))) {
            Com_Printf("DLSS-G: %s FeatureInitResult = 0x%08x\n", label, (unsigned)featureInitResult);
        }
    }
}

/* Number of GENERATED frames this device supports: 1 = 2x only, 3 = up to 4x.
   Filled in by ReportFrameGenSupport() at startup and read by the FG cvar clamp. */
unsigned int dlssgMaxGeneratedFrames = 1;

void ReportFrameGenSupport()
{
    if (!dlssObj.isInitalized || dlssObj.pParams == NULL) {
        Com_Printf("DLSS-G: NGX not initialised, cannot query frame generation\n");
        return;
    }

    ReportOneFrameGenFeature("FrameGeneration",
        NVSDK_NGX_Parameter_FrameGeneration_Available,
        NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,
        NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,
        NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor,
        NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor);

    ReportOneFrameGenFeature("FrameInterpolation",
        NVSDK_NGX_Parameter_FrameInterpolation_Available,
        NVSDK_NGX_Parameter_FrameInterpolation_FeatureInitResult,
        NVSDK_NGX_Parameter_FrameInterpolation_NeedsUpdatedDriver,
        NVSDK_NGX_Parameter_FrameInterpolation_MinDriverVersionMajor,
        NVSDK_NGX_Parameter_FrameInterpolation_MinDriverVersionMinor);

    /* Multi Frame Generation capability. MultiFrameCountMax is the number of GENERATED
       frames the driver will allow, so the user-facing multiplier is that plus one:
       1 -> 2x, 2 -> 3x, 3 -> 4x. The SDK says "If 1 or not set, multiframe is not
       supported", and an unsupported key leaves the out-param untouched, so seed it. */
    unsigned int maxGenerated = 1;
    NVSDK_NGX_Result resMFG = NVSDK_NGX_Parameter_GetUI(dlssObj.pParams,
        NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &maxGenerated);

    if (NVSDK_NGX_SUCCEED(resMFG)) {
        if (maxGenerated < 1)
            maxGenerated = 1;
        dlssgMaxGeneratedFrames = maxGenerated;
        Com_Printf("DLSS-G: MultiFrameCountMax = %u generated frame(s) -> up to %ux\n",
            maxGenerated, maxGenerated + 1);
    }
    else {
        dlssgMaxGeneratedFrames = 1;
        Com_Printf("DLSS-G: MultiFrameCountMax -> query failed (res=0x%x), assuming 2x only\n",
            (unsigned int)resMFG);
    }
}

qboolean DLSSConstructor(VkInstance _instance, VkDevice _device, VkPhysicalDevice _physDevice, const char* _pAppGuid, qboolean _enableDebug)  {

    if (dlssObj.isInitalized) {
        DLSSDeconstructor();
    }
    dlssObj.device = _device;
    dlssObj.isInitalized = qfalse;
    dlssObj.pParams = NULL;
    dlssObj.pDlssFeature = NULL;
   
    dlssObj.isInitalized = TryInit(_instance,  _physDevice, _pAppGuid, _enableDebug);

    ReportFrameGenSupport();

    if (!CheckSupport()) {
        return qfalse;
    }

    dlssObj.created = qtrue;
    return qtrue;
}

qboolean TryInit(VkInstance _instance, VkPhysicalDevice _physDevice, const char* _pAppGuid, qboolean _enableDebug) {
    NVSDK_NGX_Result res;
    const wchar_t* dllPath = GetWC(GetFolderPath());
    const wchar_t* dataPath = GetWC("DLSSTemp/");

    NVSDK_NGX_PathListInfo pathInfo = {
        .Path = &dllPath,
        .Length = 1
    };

    NVSDK_NGX_LoggingInfo loggingInfo = {
        .LoggingCallback = &DLSSPrintCallback,
        .MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON
    };

    NVSDK_NGX_FeatureCommonInfo commonInfo = {
        .PathListInfo = pathInfo,
        .LoggingInfo = loggingInfo
    };

    NVSDK_NGX_EngineType engineTypeEnum = NVSDK_NGX_ENGINE_TYPE_CUSTOM;

    res = NVSDK_NGX_VULKAN_Init_with_ProjectID(_pAppGuid, engineTypeEnum, LONG_VERSION_STRING, dataPath, _instance, _physDevice, dlssObj.device, NULL, NULL, &commonInfo, NVSDK_NGX_Version_API);

    if (NVSDK_NGX_FAILED(res)) {
        Com_EPrintf("DLSS failed init with Project id: %d", res);
        return qfalse;
    }

    res = NVSDK_NGX_VULKAN_GetCapabilityParameters(&dlssObj.pParams);

    if (NVSDK_NGX_FAILED(res))
    {
        Com_EPrintf("DLSS: NVSDK_NGX_VULKAN_GetCapabilityParameters fail: %d", res);

        NVSDK_NGX_VULKAN_Shutdown1(dlssObj.device);
        dlssObj.pParams = NULL;


        return qfalse;
    }

    return qtrue;
}

qboolean CheckSupport() {


    if (!dlssObj.isInitalized || dlssObj.pParams == NULL) {
        return qfalse;
    }

    float minDriverVersionMajor = 0;
    float minDriverVersionMinor = 0;
    float needsUpdatedDriver = 0;
   
    NVSDK_NGX_Result res_upd = NVSDK_NGX_Parameter_GetF(dlssObj.pParams, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver);
    NVSDK_NGX_Result res_mjr = NVSDK_NGX_Parameter_GetF(dlssObj.pParams, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverVersionMajor);
    NVSDK_NGX_Result res_mnr = NVSDK_NGX_Parameter_GetF(dlssObj.pParams, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverVersionMinor);

    if (NVSDK_NGX_SUCCEED(res_upd) && NVSDK_NGX_SUCCEED(res_mjr) && NVSDK_NGX_SUCCEED(res_mnr))
    {
        if (needsUpdatedDriver)
        {
            Com_EPrintf("DLSS: Can't load: Outdated driver. Min driver version: %d", minDriverVersionMinor);
            return qfalse;
        }
        else
        {
            Com_EPrintf("DLSS: Reported Min driver version: %d", minDriverVersionMinor);
        }
    }
    else
    {
        Com_EPrintf("DLSS: Minimum driver version was not reported");
    }
    
    NVSDK_NGX_Result featureInitResult;
    NVSDK_NGX_Result res;
    float isDlssSupported = 0;
    bool dldenoise = Cvar_Get("pt_dlss_dldn", "0", CVAR_ARCHIVE)->integer == 1;

    if (!dldenoise) {
        res = NVSDK_NGX_Parameter_GetF(dlssObj.pParams, NVSDK_NGX_Parameter_SuperSampling_Available, &isDlssSupported);
    }
    else {
        res = NVSDK_NGX_Parameter_GetF(dlssObj.pParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &isDlssSupported);
    }    

    if (NVSDK_NGX_FAILED(res) || !isDlssSupported) {

        if (!dldenoise) {
            res = NVSDK_NGX_Parameter_GetI(dlssObj.pParams, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, (int*)&featureInitResult);
        }
        else {
            res = NVSDK_NGX_Parameter_GetI(dlssObj.pParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, (int*)&featureInitResult);
        }
        
        if (NVSDK_NGX_SUCCEED(res))
        {
            Com_EPrintf("DLSS: Not available on this hardware/platform. FeatureInitResult=%d", (int)(featureInitResult));
        }

        return qfalse;
    }

    return qtrue;
}

void DLSSDeconstructor() {
    if (dlssObj.isInitalized) {
        vkpt_device_wait_idle();

        /* The frame generation feature shares dlssObj.pParams and the NGX instance, so
           it MUST be released before NGX is shut down. Leaving it alive here leaked the
           handle across a vid_restart and left NGX in a state where the next
           NGX_VULKAN_CREATE_DLSS_EXT failed outright ("Internal error of Nvidia DLSS").
           A vid_restart is easy to hit: several CVAR_REFRESH cvars trigger one. */
        DestroyDLSSGFeature();

        if (dlssObj.pDlssFeature != NULL) {
            DestroyDLSSFeature();
        }

        NVSDK_NGX_VULKAN_DestroyParameters(dlssObj.pParams);
        NVSDK_NGX_VULKAN_Shutdown(dlssObj.device);

        dlssObj.pParams = NULL;
        dlssObj.isInitalized = qfalse;
        dlssObj.device = NULL;
        dlssObj.created = qfalse;       
    }
}

void DestroyDLSSFeature() {
    Q_assert(dlssObj.pDlssFeature != NULL);

    vkpt_device_wait_idle();

    NVSDK_NGX_Result res = NVSDK_NGX_VULKAN_ReleaseFeature(dlssObj.pDlssFeature);
    dlssObj.pDlssFeature = NULL;

    if (NVSDK_NGX_FAILED(res))
    {
        Com_EPrintf("DLSS: NVSDK_NGX_VULKAN_ReleaseFeature fail: %d", (int)res);
    }
}

NVSDK_NGX_PerfQuality_Value ToNGXPerfQuality()
{
    NVSDK_NGX_PerfQuality_Value myValue;

    switch (cvar_pt_dlss->integer)
    {
    case -1:
        myValue = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        break;
    case 1:
        myValue = NVSDK_NGX_PerfQuality_Value_MaxPerf;   
        break;
    case 2:
        myValue = NVSDK_NGX_PerfQuality_Value_Balanced; 
        break;
    case 3:
        myValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;  
        break;
    case 4:
    case 5:
        myValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        break;
    default:
        myValue = NVSDK_NGX_PerfQuality_Value_Balanced;        
        break;
    }

    return myValue;
}

qboolean IsDLSSAvailable() {
    return dlssObj.isInitalized && dlssObj.pParams != NULL;
}

qboolean AreSameDLSSFeatureValues(struct DLSSRenderResolution resObject) {

    if (dlssModeChanged) {
        dlssModeChanged = qfalse;
        return qfalse;
    }
    qboolean res = ((dlssObj.prevDlssFeatureValues.renderWidth == resObject.inputWidth &&
        dlssObj.prevDlssFeatureValues.renderHeight == resObject.inputHeight &&
        dlssObj.prevDlssFeatureValues.upscaledWidth == resObject.outputWidth &&
        dlssObj.prevDlssFeatureValues.upscaledHeight == resObject.outputHeight) ? qtrue : qfalse);

    return res;
}

void SaveDLSSFeatureValues(struct DLSSRenderResolution resObject) 
{
    PrevDlssFeatureValues newObj = {
        .renderWidth = resObject.inputWidth,
        .renderHeight = resObject.inputHeight,
        .upscaledWidth = resObject.outputWidth,
        .upscaledHeight = resObject.outputHeight
    };
    dlssObj.prevDlssFeatureValues = newObj;
}

qboolean ValidateDLSSFeature(VkCommandBuffer cmd, struct DLSSRenderResolution resObject) {
    if (!dlssObj.isInitalized || dlssObj.pParams == NULL) {
        return qfalse;
    }

    if (AreSameDLSSFeatureValues(resObject)) {
        return qtrue;
    }

    // NOTE: the resolution is recorded only after a *successful* create, further down.
    // Saving it up front meant a single failed creation was cached as the current state,
    // so AreSameDLSSFeatureValues() short-circuited every later call and the feature was
    // never retried - turning any transient failure into a permanent fatal error.
    if (dlssObj.pDlssFeature != NULL) {
        DestroyDLSSFeature();
    }


    NVSDK_NGX_DLSSD_Create_Params denoiseParm = {
        .InWidth = resObject.inputWidth,
        .InHeight = resObject.inputHeight,
        .InTargetWidth = resObject.outputWidth,
        .InTargetHeight = resObject.outputHeight,
        .InPerfQualityValue = ToNGXPerfQuality(),
		.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified,
		.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked,
		.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear                   
        
    };

    NVSDK_NGX_DLSS_Create_Params dlssParams = {
        .Feature = {.InWidth = resObject.inputWidth,
                     .InHeight = resObject.inputHeight,
                     .InTargetWidth = resObject.outputWidth,
                     .InTargetHeight = resObject.outputHeight,
                     .InPerfQualityValue = ToNGXPerfQuality()
                   }
    };

    // Motion vectors are produced at render resolution (IMG_WIDTH_TAA == extent_render)
    // and are NOT jittered - primary_rays.rgen reprojects unjittered positions, and the
    // jitter is reported separately through InJitterOffset. So MVLowRes on, MVJittered off.
    int DlssCreateFeatureFlags = NVSDK_NGX_DLSS_Feature_Flags_None;
    DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

    // NVSDK_NGX_DLSS_Feature_Flags_Reserved_0 is exactly that - reserved (see
    // nvsdk_ngx_defs.h). Setting it is undefined behaviour, so it is not set here.

    // IsHDR is required by both features: the colour handed to DLSS is VKPT_IMG_TAA_OUTPUT,
    // which is linear HDR from before tone mapping (DLSS Programming Guide 3.5). It
    // describes the input colour space and is NOT an exposure control - Ray Reconstruction
    // refuses to be created without it ("Error: HDR Color required" from
    // NgxSwinDenoiser::CreateDldnInstance).
    //
    // AutoExposure is a different matter: that one really is unsupported by Ray
    // Reconstruction (RR integration guide 3.7 says to ignore 3.9 Exposure Parameter,
    // 3.10 Auto Exposure and 3.11 Additional Sharpening), so it is only set for
    // Super Resolution.
    int SuperSamplingCreateFlags = DlssCreateFeatureFlags
                                 | NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                                 | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    int RayReconstructionCreateFlags = DlssCreateFeatureFlags
                                 | NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    dlssParams.InFeatureCreateFlags = SuperSamplingCreateFlags;
    denoiseParm.InFeatureCreateFlags = RayReconstructionCreateFlags;

    // only one phys device
    uint32_t creationNodeMask = 1;
    uint32_t visibilityNodeMask = 1;    
    bool denoiseMode = Cvar_Get("pt_dlss_dldn", "0", CVAR_ARCHIVE)->integer == 1;

    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_DLSS_Hint_Render_Preset_M);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, NVSDK_NGX_DLSS_Hint_Render_Preset_K);

    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F);
    
    //NVSDK_NGX_Parameter_SetF(dlssObj.pParams, NVSDK_NGX_Parameter_Hint_UseFireflySwatter, 1.0f);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_Denoise, 1);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Unpacked);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_Use_HW_Depth, NVSDK_NGX_DLSS_Depth_Type_Linear);

    NVSDK_NGX_Result res;

    // NGX_VULKAN_CREATE_DLSS_EXT / NGX_VULKAN_CREATE_DLSSD_EXT1 already call
    // NVSDK_NGX_VULKAN_CreateFeature(1) internally with the right feature id. Calling
    // CreateFeature again afterwards created a *second* feature and overwrote the
    // handle, leaking the first one on every resolution / mode change, and left `res`
    // describing the handle that had just been thrown away.
    if (!denoiseMode) {
        res = NGX_VULKAN_CREATE_DLSS_EXT(cmd, creationNodeMask, visibilityNodeMask, &dlssObj.pDlssFeature, dlssObj.pParams, &dlssParams);
    }
    else {
        res = NGX_VULKAN_CREATE_DLSSD_EXT1(dlssObj.device, cmd, creationNodeMask, visibilityNodeMask, &dlssObj.pDlssFeature, dlssObj.pParams, &denoiseParm);
    }
  
    if (NVSDK_NGX_FAILED(res))
    {
        // The NGX feature logs in the data path (DLSSTemp/) carry the actual reason,
        // e.g. "Error: HDR Color required" from NgxSwinDenoiser::CreateDldnInstance.
        Com_EPrintf("DLSS: %s feature create failed: 0x%08x - see DLSSTemp/nvngx*.log\n",
            denoiseMode ? "RayReconstruction" : "SuperSampling", (unsigned int)res);

        dlssObj.pDlssFeature = NULL;
        return qfalse;
    }

    SaveDLSSFeatureValues(resObject);

    return qtrue;
}

NVSDK_NGX_Resource_VK ToNGXResource(VkImage image, VkImageView imageView, NVSDK_NGX_Dimensions size, VkFormat format, bool withWriteAccess) {
    VkImageSubresourceRange subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    
    return NVSDK_NGX_Create_ImageView_Resource_VK(imageView, image, subresourceRange, format, size.Width, size.Height, withWriteAccess);
}

NVSDK_NGX_Resource_VK ToNGXBufferResource(VkBuffer buffer, size_t bufferSize, bool withWriteAccess) {
    return NVSDK_NGX_Create_Buffer_Resource_VK(buffer, bufferSize, withWriteAccess);
}

void DLSSApply(VkCommandBuffer cmd,  QVK_t qvk, struct DLSSRenderResolution resObject, vec2 jitterOffset, float timeDelta, qboolean resetAccum) {
    if (!IsDLSSAvailable())
    {
        Com_Error(ERR_FATAL, "Nvidia DLSS is not supported (or DLSS dynamic library files are notfound). Check availability before usage.");
    }

    if (ToNGXPerfQuality() == NVSDK_NGX_PerfQuality_Value_DLAA) {
        resObject.outputWidth = resObject.inputWidth;
        resObject.outputHeight = resObject.inputHeight;
    }

    ValidateDLSSFeature(cmd, resObject);

    if (dlssObj.pDlssFeature == NULL)
    {
        Com_Error(ERR_FATAL, "Internal error of Nvidia DLSS: NGX_VULKAN_CREATE_DLSS_EXT has failed.");
    }

    //qvk.images[]
    int frame_idx = qvk.frame_counter & 1;
    bool denoiseMode = Cvar_Get("pt_dlss_dldn", "0", CVAR_ARCHIVE)->integer == 1;
    NVSDK_NGX_Coordinates sourceOffset = { 0, 0 };
    NVSDK_NGX_Dimensions  sourceSize = {
        resObject.inputWidth,
        resObject.inputHeight,
    };

    NVSDK_NGX_Dimensions targetSize = {
        resObject.outputWidth,
        resObject.outputHeight,
    };    

    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_TAA_OUTPUT]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_OUTPUT]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_PT_DLSS_MOTION]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_RAY_LENGTH]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_DEPTH]);
    //BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_TRANSPARENT]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_PT_MOTION]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_PT_REFLECT_MOTION]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_ALBEDO]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_SPECULAR]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_ROUGHNESS]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_METALLIC]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_NORMAL]);
    //BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_MATERIALID]);
    //BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_EMISSIVE]);
    //BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_INDIRECT_ALBEDO]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_SPECULAR_ALBEDO]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_BEFORE_TRANSPARENT]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_REFLECTED_ALBEDO]);

    BUFFER_BARRIER(cmd,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
        .buffer = qvk.buf_world.buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
        );

    NVSDK_NGX_Resource_VK unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_TAA_OUTPUT], qvk.images_views[VKPT_IMG_TAA_OUTPUT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK motionVectorsResource = ToNGXResource(qvk.images[VKPT_IMG_PT_DLSS_MOTION], qvk.images_views[VKPT_IMG_PT_DLSS_MOTION], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK resolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_OUTPUT], qvk.images_views[VKPT_IMG_DLSS_OUTPUT], targetSize, VK_FORMAT_R16G16B16A16_SFLOAT, true);    
    NVSDK_NGX_Resource_VK depthResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_DEPTH], qvk.images_views[VKPT_IMG_DLSS_DEPTH], sourceSize, VK_FORMAT_R32_SFLOAT, false);
    NVSDK_NGX_Resource_VK reflectMotion = ToNGXResource(qvk.images[VKPT_IMG_DLSS_REFLECT_MOTION], qvk.images_views[VKPT_IMG_DLSS_REFLECT_MOTION], sourceSize, VK_FORMAT_R32G32_SFLOAT, false);
    NVSDK_NGX_Resource_VK albedo = ToNGXResource(qvk.images[VKPT_IMG_DLSS_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK specular = ToNGXResource(qvk.images[VKPT_IMG_DLSS_SPECULAR], qvk.images_views[VKPT_IMG_DLSS_SPECULAR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK roughness = ToNGXResource(qvk.images[VKPT_IMG_DLSS_ROUGHNESS], qvk.images_views[VKPT_IMG_DLSS_ROUGHNESS], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK metallic = ToNGXResource(qvk.images[VKPT_IMG_DLSS_METALLIC], qvk.images_views[VKPT_IMG_DLSS_METALLIC], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK normal = ToNGXResource(qvk.images[VKPT_IMG_DLSS_NORMAL], qvk.images_views[VKPT_IMG_DLSS_NORMAL], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK specularAlbedo = ToNGXResource(qvk.images[VKPT_IMG_DLSS_SPECULAR_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_SPECULAR_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK beforeTransparent = ToNGXResource(qvk.images[VKPT_IMG_DLSS_BEFORE_TRANSPARENT], qvk.images_views[VKPT_IMG_DLSS_BEFORE_TRANSPARENT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK diffuseLength = ToNGXResource(qvk.images[VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE], qvk.images_views[VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK specularLength = ToNGXResource(qvk.images[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR], qvk.images_views[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);

    NVSDK_NGX_VK_GBuffer inBuffer = {
        .pInAttrib[NVSDK_NGX_GBUFFER_ALBEDO] = &albedo        
    };

    switch (Cvar_Get("pt_dlss_debug", "0", CVAR_ARCHIVE)->integer) {
        case 1:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_RAY_LENGTH], qvk.images_views[VKPT_IMG_DLSS_RAY_LENGTH], sourceSize, VK_FORMAT_R32G32B32A32_SFLOAT, false);
            break;
        case 2:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_3DMOTION_VECTOR], qvk.images_views[VKPT_IMG_DLSS_3DMOTION_VECTOR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 3:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_REFLECT_MOTION], qvk.images_views[VKPT_IMG_DLSS_REFLECT_MOTION], sourceSize, VK_FORMAT_R32G32_SFLOAT, false);
            break;
        case 4:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 5:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_SPECULAR], qvk.images_views[VKPT_IMG_DLSS_SPECULAR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 6:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_ROUGHNESS], qvk.images_views[VKPT_IMG_DLSS_ROUGHNESS], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 7:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_METALLIC], qvk.images_views[VKPT_IMG_DLSS_METALLIC], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 8:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_NORMAL], qvk.images_views[VKPT_IMG_DLSS_NORMAL], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 9:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_MATERIALID], qvk.images_views[VKPT_IMG_DLSS_MATERIALID], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 10:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_EMISSIVE], qvk.images_views[VKPT_IMG_DLSS_EMISSIVE], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 11:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_INDIRECT_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_INDIRECT_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 12:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_SPECULAR_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_SPECULAR_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 13:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_TRANSPARENT], qvk.images_views[VKPT_IMG_DLSS_TRANSPARENT], sourceSize, VK_FORMAT_R32G32B32A32_SFLOAT, false);
            break;
        case 14:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_DEPTH], qvk.images_views[VKPT_IMG_DLSS_DEPTH], sourceSize, VK_FORMAT_R32_SFLOAT, false);
            break;
        case 15:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_PT_DLSS_MOTION], qvk.images_views[VKPT_IMG_PT_DLSS_MOTION], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 16:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_BEFORE_TRANSPARENT], qvk.images_views[VKPT_IMG_DLSS_BEFORE_TRANSPARENT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 17:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE], qvk.images_views[VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 18:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR], qvk.images_views[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 19:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_PT_TRANSPARENT], qvk.images_views[VKPT_IMG_PT_TRANSPARENT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 20:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_PT_MOTION], qvk.images_views[VKPT_IMG_PT_MOTION], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 21:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_PT_DLSS_MOTION], qvk.images_views[VKPT_IMG_PT_DLSS_MOTION], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 22:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_ASVGF_HIST_COLOR_HF], qvk.images_views[VKPT_IMG_ASVGF_HIST_COLOR_HF], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 23:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_PT_SHADING_POSITION], qvk.images_views[VKPT_IMG_PT_SHADING_POSITION], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 24:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_FLAT_COLOR], qvk.images_views[VKPT_IMG_FLAT_COLOR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 25:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_FLAT_MOTION], qvk.images_views[VKPT_IMG_FLAT_MOTION], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 26:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_TAA_OUTPUT], qvk.images_views[VKPT_IMG_TAA_OUTPUT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 27:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_PT_THROUGHPUT], qvk.images_views[VKPT_IMG_PT_THROUGHPUT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 28:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_PT_BOUNCE_THROUGHPUT], qvk.images_views[VKPT_IMG_PT_BOUNCE_THROUGHPUT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 29:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_HQ_COLOR_INTERLEAVED], qvk.images_views[VKPT_IMG_HQ_COLOR_INTERLEAVED], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 30:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_REFLECTED_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_REFLECTED_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;

    }


    // Jitter.
    //
    // The programming guide (3.7.3) asks for "the jitter applied to the projection
    // matrix". Q2RTX never jitters a projection matrix - primary_rays.rgen samples at
    // (pixel_centre + sub_pixel_jitter) instead. Those are not the same sign: sampling
    // toward +j makes scene content land at -j in the image, so the equivalent
    // projection jitter is -sub_pixel_jitter. Hence the negation, which is also what
    // stock Q2RTX did.
    //
    // The axes themselves already agree and need no per-component flip: the jitter is in
    // y-down pixel space (image_position is a launch index) and projection_view_to_screen
    // produces y-down UVs, which is why asvgf_temporal.comp can add motion.xy straight
    // onto a y-down pixel coordinate.
    //
    // To settle it empirically rather than by argument, the development nvngx_dlssd.dll
    // remaps the jitter live: CTRL+ALT+F9 cycles the configurations in the SDK's
    // utils/DLSS_Debug_Jitter_Configs.txt (0 = as sent, 9 = negate Y, 10 = negate X,
    // 11 = negate both) and CTRL+ALT+F10 swaps X and Y. Set pt_dlss_jitter_sign 1 first
    // so the engine sends the raw offset, then cycle.
    float jitterSign = (float)Cvar_Get("pt_dlss_jitter_sign", "-1", CVAR_ARCHIVE)->integer;
    if (jitterSign != 1.0f && jitterSign != -1.0f)
        jitterSign = 1.0f;

    const float jitterX = jitterOffset[0] * jitterSign;
    const float jitterY = jitterOffset[1] * jitterSign;

    const bool useDiffuseHitDist = (cvar_pt_dlss_diff_hitdist != NULL)
        && cvar_pt_dlss_diff_hitdist->integer != 0;

    const bool useReflectedAlbedo = (cvar_pt_dlss_reflected_albedo != NULL)
        && cvar_pt_dlss_reflected_albedo->integer != 0;
    NVSDK_NGX_Resource_VK reflectedAlbedo = ToNGXResource(qvk.images[VKPT_IMG_DLSS_REFLECTED_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_REFLECTED_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);

    if (!denoiseMode) {
        NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {
            .Feature = {.pInColor = &unresolvedColorResource, .pInOutput = &resolvedColorResource },
            .pInDepth = &depthResource,
            .pInMotionVectors = &motionVectorsResource,
            .InJitterOffsetX = jitterX,
            .InJitterOffsetY = jitterY,
            .InRenderSubrectDimensions = sourceSize,
            .InReset = resetAccum ? 1 : 0,
            .InMVScaleX = sourceSize.Width,
            .InMVScaleY = sourceSize.Height,
            .InColorSubrectBase = sourceOffset,
            .InDepthSubrectBase = sourceOffset,
            .InMVSubrectBase = sourceOffset
        };

        NVSDK_NGX_Result res = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, dlssObj.pDlssFeature, dlssObj.pParams, &evalParams);
        if (NVSDK_NGX_FAILED(res))
        {
            Com_EPrintf("DLSS: NGX_VULKAN_EVALUATE_DLSS_EXT fail: %d", (int)res);
        }
    }
    else {

        NVSDK_NGX_VK_DLSSD_Eval_Params evalParams = {
			.pInColor = &unresolvedColorResource,
			.pInOutput = &resolvedColorResource,
            .pInDepth = &depthResource,
            .pInMotionVectors = &motionVectorsResource,
            .InJitterOffsetX = jitterX,
            .InJitterOffsetY = jitterY,
            .InRenderSubrectDimensions = sourceSize,
            .InReset = resetAccum ? 1 : 0,
            .InMVScaleX = sourceSize.Width,
            .InMVScaleY = sourceSize.Height,
            .InColorSubrectBase = sourceOffset,
            .InDepthSubrectBase = sourceOffset,
            .InMVSubrectBase = sourceOffset,
            .pInMotionVectorsReflections = &reflectMotion,			
			.pInNormals = &normal,
			.pInDiffuseHitDistance = useDiffuseHitDist ? &diffuseLength : NULL,
			.pInSpecularHitDistance = &specularLength,
			.pInSpecularAlbedo = &specularAlbedo,
			.pInDiffuseAlbedo = &albedo,
            .pInRoughness = &roughness,
            .pInReflectedAlbedo = useReflectedAlbedo ? &reflectedAlbedo : NULL,
            .InReflectedAlbedoSubrectBase = sourceOffset,
            .pInColorBeforeTransparency = &beforeTransparent,
            .InColorBeforeTransparencySubrectBase = sourceOffset
        };

		NVSDK_NGX_Result res = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, dlssObj.pDlssFeature, dlssObj.pParams, &evalParams);
    } 
   
}

char* GetDLSSVulkanInstanceExtensions()
{
    uint32_t     instanceExtCount;
    const char** ppInstanceExts;
    uint32_t     deviceExtCount;
    const char** ppDeviceExts;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(
        &instanceExtCount, &ppInstanceExts, &deviceExtCount, &ppDeviceExts);
    if (!NVSDK_NGX_SUCCEED(r))
    {
        Com_Error(ERR_FATAL, "No ray tracing capable GPU found.");
    }

    char* v = GetEmptyString(256);
    char* vStart = v;

    for (uint32_t i = 0; i < instanceExtCount; i++)
    {
        int stringLength = strlen(ppDeviceExts[i]);

        memcpy(v, ppDeviceExts[i], stringLength);
        v += stringLength;
        *v = ';';
        v++;
    }

    return vStart;
}

char* GetDLSSVulkanDeviceExtensions()
{
    uint32_t     instanceExtCount;
    const char** ppInstanceExts;
    uint32_t     deviceExtCount;
    const char** ppDeviceExts;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(
        &instanceExtCount, &ppInstanceExts, &deviceExtCount, &ppDeviceExts);
    if (!NVSDK_NGX_SUCCEED(r))
    {
        Com_Error(ERR_FATAL, "No ray tracing capable GPU found.");
    }

    char* v = GetEmptyString(256);
    char* vStart = v;


    for (uint32_t i = 0; i < deviceExtCount; i++)
    {
        if (strcmp(ppDeviceExts[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0)
        {
            continue;
        }
        int stringLength = strlen(ppDeviceExts[i]);

        memcpy(v, ppDeviceExts[i], stringLength);
        v += stringLength;
        *v = ';';
        v++;
    }

    return vStart;
}

char* GetFolderPath()
{
    wchar_t appPath[MAX_OSPATH];
    GetModuleFileNameW(NULL, appPath, MAX_OSPATH);

    char* curFolderPath = (char*)&appPath;
    char* outFolderPath = GetEmptyString(MAX_OSPATH);
    int index = 0;
    int lastIndex = 0;

    while (curFolderPath && *curFolderPath != '\0') {
        if (*curFolderPath == '\\') {
            lastIndex = index;
        }
        curFolderPath += 2;
        index++;
    }

    wcstombs(outFolderPath, &appPath, lastIndex);

    return outFolderPath;
}

void viewsize_changed(cvar_t* self) {

    recreateSwapChain = qtrue;

    switch (self->integer) {
    case -1:
        Cvar_SetInteger(scr_viewsize, 25, FROM_MENU);
        break;
    case 1:
        Cvar_SetInteger(scr_viewsize, 50, FROM_MENU);
        break;
    case 2:
        Cvar_SetInteger(scr_viewsize, 59, FROM_MENU);
        break;
    case 3:
        Cvar_SetInteger(scr_viewsize, 66, FROM_MENU);   
        break;
    case 4:
    case 0:
    case 5:
        Cvar_SetInteger(scr_viewsize, 100, FROM_MENU);
        break;
    }

    oldCvarValue = self->integer;
    Cvar_SetByVar(vid_rtx, "0", FROM_MENU);
    Cvar_SetByVar(vid_rtx, "1", FROM_MENU);
}

void DlssModeChanged(cvar_t* self) {
    recreateSwapChain = qtrue;
    dlssModeChanged = qtrue;

    switch (cvar_pt_dlss->integer) {
    case -1:
        Cvar_SetInteger(scr_viewsize, 25, FROM_MENU);
        return;
    case 1:
        Cvar_SetInteger(scr_viewsize, 50, FROM_MENU);
        return;
    case 2:
        Cvar_SetInteger(scr_viewsize, 59, FROM_MENU);
        return;
    case 3:
        Cvar_SetInteger(scr_viewsize, 66, FROM_MENU);
        return;
    case 4:
    case 5:
    case 0:
        Cvar_SetInteger(scr_viewsize, 100, FROM_MENU);
        return;
    }
}

qboolean DLSSChanged() {
    return recreateSwapChain;
}

void DLSSSwapChainRecreated() {
    recreateSwapChain = qfalse;
}

void DLSSPrintCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent) {
    Com_EPrintf(message);
}

const wchar_t* GetWC(const char* c)
{
    const size_t cSize = strlen(c) + 1;
    wchar_t* wc = malloc(sizeof(wchar_t) * cSize);
    mbstowcs(wc, c, cSize);

    return wc;
}

/* ------------------------------------------------------------------ */
/* DLSS Frame Generation (DLSS-G)                                       */
/*                                                                      */
/* Prototype: creates and evaluates the feature and writes the          */
/* interpolated frame into VKPT_IMG_DLSS_FG_OUTPUT. It does NOT yet     */
/* pace or present that frame - `pt_dlss_fg_show 1` simply blits the    */
/* interpolated image instead of the real one so the output can be      */
/* judged by eye. Real frame generation needs a second, timed present;  */
/* see the notes in R_EndFrame_RTX.                                     */
/* ------------------------------------------------------------------ */

static NVSDK_NGX_Handle* dlssgFeature = NULL;
static uint32_t dlssgWidth = 0;
static uint32_t dlssgHeight = 0;
/* The RENDER extent the feature was created for. Tracked separately from the output
   extent because changing the DLSS mode (Quality -> DLAA, say) changes the render
   resolution while the output resolution stays put. The feature is created with
   DynamicResolutionScaling = false, so continuing to feed it depth and motion vectors
   at a different size than it was promised faults inside NGX. */
static uint32_t dlssgRenderWidth = 0;
static uint32_t dlssgRenderHeight = 0;
/* One hard failure is enough - without this a failing create is retried every
   frame, which spams the log and stalls on vkDeviceWaitIdle each time. */
static qboolean dlssgFailed = qfalse;

static cvar_t* cvar_pt_dlss_fg = NULL;
static cvar_t* cvar_pt_dlss_fg_show = NULL;
/* Multiplier applied to VKPT_IMG_PT_DLSS_MOTION before DLSS-G reads it.
   0 (the default) means "use the render extent", which is what the Super Resolution
   path does - see the note where it is used. Any other value is used literally, so the
   old behaviour is `pt_dlss_fg_mvscale 1`. */
static cvar_t* cvar_pt_dlss_fg_mvscale = NULL;

void InitDLSSGCvars()
{
    /* 0 = off. Otherwise the MULTIPLIER: 2 = 2x (one generated frame), up to
       6 = 6x on hardware that allows it. 1 is accepted as a synonym for 2 so the
       original on/off meaning of this cvar keeps working. */
    cvar_pt_dlss_fg = Cvar_Get("pt_dlss_fg", "0", CVAR_ARCHIVE);

    /* Debug view: blit the INTERPOLATED frame to the swapchain instead of the real
       one. This is not frame generation - it shows what the interpolator produces so
       its quality can be judged before the present-pacing work is done. Expect the
       image to sit roughly half a frame behind the real one. */
    cvar_pt_dlss_fg_show = Cvar_Get("pt_dlss_fg_show", "0", 0);

    cvar_pt_dlss_fg_mvscale = Cvar_Get("pt_dlss_fg_mvscale", "0", CVAR_ARCHIVE);

    /* The lowest RENDER rate a multiplier is allowed to leave. The render rate is
       what the game samples input at, and it is pinned at refresh / multiplier -
       see DLSSGDisplayMaxMultiplier(). Registered here so it exists before any
       frame with FG active has run, otherwise typing it in the console gets
       "unknown command". 0 disables the cap. */
    Cvar_Get("pt_dlss_fg_min_base", "30", CVAR_ARCHIVE);
}

unsigned int DLSSGMaxMultiplier()
{
    /* dlssgMaxGeneratedFrames is what the driver reported; the images only exist for
       DLSSG_MAX_GENERATED_FRAMES of them. */
    unsigned int generated = dlssgMaxGeneratedFrames;
    if (generated > DLSSG_MAX_GENERATED_FRAMES)
        generated = DLSSG_MAX_GENERATED_FRAMES;
    if (generated < 1)
        generated = 1;
    return generated + 1;
}

/* THE DISPLAY, NOT THE DRIVER, SETS THE USEFUL MULTIPLIER.

   The present schedule spaces every rendered frame's group over `multiplier` refresh
   intervals (main.c: slot = the vblank period, group = slot * total), so the presented
   rate saturates at the refresh rate and the RENDER rate is pinned at refresh /
   multiplier. Reflex_FGRenderRateCap() says the same thing from the other side. The game
   samples input once per RENDERED frame, so on a fixed 60 Hz panel:

     2x -> 30 rendered / 60 presented   - 30 Hz input, and the display is already full
     4x -> 15 rendered / 60 presented   - 15 Hz input, SAME 60 on screen
     6x -> 10 rendered / 60 presented   - 10 Hz input, SAME 60 on screen

   Above 2x there is no headroom left to sell: every extra generated frame is paid for by
   dividing the input rate, and DLSS-G has to bridge a wider gap from a slower base, so
   the interpolation gets worse too. Matt measured exactly this - "above 2x the input
   framerate goes down and the smoothness gets worse". MFG 3x-6x is for 120/144/240 Hz
   panels, where the extra presents have somewhere to go.

   So cap the multiplier at refresh / pt_dlss_fg_min_base. 60 Hz -> 2x, 120 Hz -> 4x,
   144 Hz -> 4x, 240 Hz -> 6x (the driver max). pt_dlss_fg_min_base 0 disables the cap for
   anyone who wants to see it for themselves.

   THE REFRESH READING IS LATCHED, and that is not fussiness. DLSSGMultiplier() feeds
   desired_swapchain_images() and the per-frame swapchain-recreate check, so a multiplier
   that flickers between two values recreates the swapchain every single frame. A reading
   of 0 (no window yet, display index lost) must therefore never lower the cap - keep the
   last valid one. */
static unsigned int DLSSGDisplayMaxMultiplier(void)
{
    static cvar_t *cvar_min_base = NULL;
    static int cached_refresh_hz = 0;   /* last VALID reading; never cleared */

    if (cvar_min_base == NULL)
        cvar_min_base = Cvar_Get("pt_dlss_fg_min_base", "30", CVAR_ARCHIVE);

    if (cvar_min_base->integer <= 0)
        return 0;               /* cap disabled */

    int refresh_hz = Reflex_DisplayRefreshHz();
    if (refresh_hz > 0)
        cached_refresh_hz = refresh_hz;
    if (cached_refresh_hz <= 0)
        return 0;               /* nothing known yet - do not cap on a guess */

    unsigned int allowed = (unsigned int)(cached_refresh_hz / cvar_min_base->integer);
    if (allowed < 2)
        allowed = 2;            /* 2x is always on the table */
    return allowed;
}

unsigned int DLSSGMultiplier()
{
    if (!DLSSGEnabled())
        return 0;

    int requested = cvar_pt_dlss_fg->integer;
    if (requested <= 1)
        requested = 2;          /* 1 kept as a synonym for 2x */

    unsigned int maxMult = DLSSGMaxMultiplier();
    if ((unsigned int)requested > maxMult)
        requested = (int)maxMult;

    unsigned int displayMax = DLSSGDisplayMaxMultiplier();
    if (displayMax > 0 && (unsigned int)requested > displayMax) {
        static int announced_for = 0;
        if (announced_for != cvar_pt_dlss_fg->integer) {
            announced_for = cvar_pt_dlss_fg->integer;
            Com_Printf("DLSS-G: %dx requested but this display can only show %u distinct "
                "frames per rendered frame, so %ux is the most it can use. Above that the "
                "presented rate is unchanged and only the render rate - and with it the "
                "input rate - goes down. Running %ux. Set pt_dlss_fg_min_base 0 to "
                "override.\n",
                cvar_pt_dlss_fg->integer, displayMax, displayMax, displayMax);
        }
        requested = (int)displayMax;
    }

    return (unsigned int)requested;
}

static unsigned int dlssgFrameBudget = 0;   /* 0 = no limit */

void DLSSGSetFrameBudget(unsigned int maxGeneratedFrames)
{
    dlssgFrameBudget = maxGeneratedFrames;
}

unsigned int DLSSGGeneratedFrames()
{
    unsigned int mult = DLSSGMultiplier();
    unsigned int generated = mult ? mult - 1 : 0;

    /* Generating frames the display cannot show is worse than not generating them: each
       one costs an Evaluate and a present, and the display then samples the group at
       whatever phase it happens to be in. See the budget note in main.c. */
    if (dlssgFrameBudget && generated > dlssgFrameBudget)
        generated = dlssgFrameBudget;

    return generated;
}

qboolean DLSSGEnabled()
{
    if (dlssgFailed || cvar_pt_dlss_fg == NULL)
        return qfalse;

    /* The backbuffer we hand DLSS-G is VKPT_IMG_DLSS_OUTPUT, so frame generation only
       makes sense while DLSS itself is producing that image. */
    return DLSSEnabled() && cvar_pt_dlss_fg->integer != 0;
}

qboolean DLSSGFeatureReady()
{
    return dlssgFeature != NULL ? qtrue : qfalse;
}

qboolean DLSSGShowInterpolated()
{
    return dlssgFeature != NULL
        && cvar_pt_dlss_fg_show != NULL
        && cvar_pt_dlss_fg_show->integer != 0;
}

/* Output image for generated frame `generatedIndex` (1-based). One image per generated
   frame, because they all have to stay live until each has been presented. */
static int DLSSGOutputImageIndex(unsigned int generatedIndex)
{
    switch (generatedIndex) {
    case 2:  return VKPT_IMG_DLSS_FG_OUTPUT2;
    case 3:  return VKPT_IMG_DLSS_FG_OUTPUT3;
    case 4:  return VKPT_IMG_DLSS_FG_OUTPUT4;
    case 5:  return VKPT_IMG_DLSS_FG_OUTPUT5;
    default: return VKPT_IMG_DLSS_FG_OUTPUT;
    }
}

VkImage GetDLSSGImage(unsigned int generatedIndex)
{
    return qvk.images[DLSSGOutputImageIndex(generatedIndex)];
}

void DestroyDLSSGFeature()
{
    dlssgWidth = 0;
    dlssgHeight = 0;
    dlssgRenderWidth = 0;
    dlssgRenderHeight = 0;

    if (dlssgFeature != NULL) {
        vkpt_device_wait_idle();

        NVSDK_NGX_Result res = NVSDK_NGX_VULKAN_ReleaseFeature(dlssgFeature);
        if (NVSDK_NGX_FAILED(res)) {
            Com_EPrintf("DLSS-G: ReleaseFeature failed: 0x%08x\n", (unsigned int)res);
        }
        dlssgFeature = NULL;
    }

    dlssgWidth = 0;
    dlssgHeight = 0;
}

static qboolean ValidateDLSSGFeature(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                     uint32_t renderWidth, uint32_t renderHeight)
{
    if (!dlssObj.isInitalized || dlssObj.pParams == NULL)
        return qfalse;

    if (dlssgFeature != NULL
        && dlssgWidth == width && dlssgHeight == height
        && dlssgRenderWidth == renderWidth && dlssgRenderHeight == renderHeight)
        return qtrue;

    if (dlssgFeature != NULL)
        DestroyDLSSGFeature();

    /* The "native backbuffer format" is the format of the colour we hand over, which
       is VKPT_IMG_DLSS_OUTPUT - rgba16f, not the swapchain format, because the
       swapchain image is not written until the final blit. */
    unsigned int res = DLSSG_CreateFeature(cmd, dlssObj.pParams, width, height,
                                           renderWidth, renderHeight,
                                           VK_FORMAT_R16G16B16A16_SFLOAT, &dlssgFeature);

    if (res != (unsigned int)NVSDK_NGX_Result_Success) {
        Com_EPrintf("DLSS-G: feature create failed: 0x%08x - see DLSSTemp/nvngx_dlssg*.log\n", res);
        dlssgFeature = NULL;
        dlssgFailed = qtrue;
        return qfalse;
    }

    dlssgWidth = width;
    dlssgHeight = height;
    dlssgRenderWidth = renderWidth;
    dlssgRenderHeight = renderHeight;
    Com_Printf("DLSS-G: frame generation feature created at %ux%u (render %ux%u)\n",
        width, height, renderWidth, renderHeight);
    return qtrue;
}

/* Q2RTX stores 4x4 matrices column-major (the translation lives in elements 12..14,
   see the viewport_proj literal in main.c). NGX documents its matrices as
   float[4][4] without stating the order; NVIDIA's own samples feed row-major.
   THIS IS THE FIRST THING TO FLIP if the interpolated frame looks structurally
   wrong while depth and motion vectors are visibly fine. */
static void mat4_transpose(float* dst, const float* src)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            dst[r * 4 + c] = src[c * 4 + r];
        }
    }
}

void DLSSGApply(VkCommandBuffer cmd, qboolean resetAccum)
{
    if (!DLSSGEnabled())
        return;

    const uint32_t outWidth = qvk.extent_unscaled.width;
    const uint32_t outHeight = qvk.extent_unscaled.height;

    if (!ValidateDLSSGFeature(cmd, outWidth, outHeight,
                              qvk.extent_render.width, qvk.extent_render.height))
        return;

    QVKUniformBuffer_t* ubo = &vkpt_refdef.uniform_buffer;

    /* clipToPrevClip / prevClipToClip are built from the view-projection pair rather
       than from V/P separately, because the UBO carries V_prev and P_prev but no
       inverse of V_prev. */
    mat4_t VP, VP_prev, invVP, invVP_prev, clipToPrev, prevToClip;
    mult_matrix_matrix(VP, *ubo->P, *ubo->V);
    mult_matrix_matrix(VP_prev, *ubo->P_prev, *ubo->V_prev);
    inverse(VP, invVP);
    inverse(VP_prev, invVP_prev);
    mult_matrix_matrix(clipToPrev, VP_prev, invVP);
    mult_matrix_matrix(prevToClip, VP, invVP_prev);

    float mViewToClip[16], mClipToView[16], mClipToPrev[16], mPrevToClip[16];
    mat4_transpose(mViewToClip, *ubo->P);
    mat4_transpose(mClipToView, *ubo->invP);
    mat4_transpose(mClipToPrev, clipToPrev);
    mat4_transpose(mPrevToClip, prevToClip);

    NVSDK_NGX_Dimensions outSize = { outWidth, outHeight };
    NVSDK_NGX_Dimensions renderSize = {
        qvk.extent_render.width,
        qvk.extent_render.height
    };

    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_OUTPUT]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_DEPTH]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_PT_DLSS_MOTION]);

    NVSDK_NGX_Resource_VK backbuffer = ToNGXResource(
        qvk.images[VKPT_IMG_DLSS_OUTPUT], qvk.images_views[VKPT_IMG_DLSS_OUTPUT],
        outSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);

    NVSDK_NGX_Resource_VK depth = ToNGXResource(
        qvk.images[VKPT_IMG_DLSS_DEPTH], qvk.images_views[VKPT_IMG_DLSS_DEPTH],
        renderSize, VK_FORMAT_R32_SFLOAT, false);

    NVSDK_NGX_Resource_VK mvecs = ToNGXResource(
        qvk.images[VKPT_IMG_PT_DLSS_MOTION], qvk.images_views[VKPT_IMG_PT_DLSS_MOTION],
        renderSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);

    float fovYRadians = 0.0f;
    float aspect = (outHeight > 0) ? ((float)outWidth / (float)outHeight) : 1.0f;
    if (vkpt_refdef.fd) {
        fovYRadians = vkpt_refdef.fd->fov_y * (float)M_PI / 180.0f;
    }

    float fgJitterSign = (float)Cvar_Get("pt_dlss_jitter_sign", "-1", CVAR_ARCHIVE)->integer;
    if (fgJitterSign != 1.0f && fgJitterSign != -1.0f)
        fgJitterSign = 1.0f;

    float fgMvecScaleX = (float)renderSize.Width;
    float fgMvecScaleY = (float)renderSize.Height;
    if (cvar_pt_dlss_fg_mvscale && cvar_pt_dlss_fg_mvscale->value != 0.0f) {
        fgMvecScaleX = cvar_pt_dlss_fg_mvscale->value;
        fgMvecScaleY = cvar_pt_dlss_fg_mvscale->value;
    }

    /* DLSS-G uses this to tell that real frames are advancing. The SDK marks it
       optional, but it costs nothing and removes a guess. */
    const int fgMvecSign = Cvar_Get("pt_dlss_fg_mvsign", "1", 0)->integer < 0 ? -1 : 1;

    NVSDK_NGX_Parameter_SetULL(dlssObj.pParams,
        NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, (unsigned long long)qvk.frame_counter);

    /* Multi Frame Generation: Evaluate runs once per generated frame, each writing its
       own output image, with multiFrameIndex counting 1..multiFrameCount. */
    const unsigned int generatedFrames = DLSSGGeneratedFrames();

    for (unsigned int genIndex = 1; genIndex <= generatedFrames; genIndex++)
    {
    const int outImg = DLSSGOutputImageIndex(genIndex);
    BARRIER_COMPUTE(cmd, qvk.images[outImg]);

    NVSDK_NGX_Resource_VK outputInterp = ToNGXResource(
        qvk.images[outImg], qvk.images_views[outImg],
        outSize, VK_FORMAT_R16G16B16A16_SFLOAT, true);

    DLSSG_EvalInputs in = {
        .pBackbuffer = &backbuffer,
        .pDepth = &depth,
        .pMotionVectors = &mvecs,
        .pOutputInterpolated = &outputInterp,

        .renderWidth = renderSize.Width,
        .renderHeight = renderSize.Height,

        /* MEASURED THE HARD WAY. The Super Resolution path passes
           InMVScaleX/Y = the render extent (DLSS.c:894), which means
           VKPT_IMG_PT_DLSS_MOTION holds NORMALIZED motion and the scale is what turns
           it into pixels. DLSS-G's mvecScale is the same kind of multiplier, so it
           needs the render extent too.

           This was 1.0 at first, on the theory that DLSS-G wanted vectors already in
           [-1,1]. That made the applied motion ~3000x too small, so every interpolated
           frame came out a near-copy of one endpoint: the presented frame rate doubled
           while the motion still stepped at the render rate, which looks SLOWER than
           frame generation switched off, and only a very fast camera produced enough
           motion to show any warping at all. Matt spotted it as "the movement doesn't
           look smooth" while the counter read double.

           pt_dlss_fg_mvscale overrides it so the convention stays testable rather than
           re-derived: 0 = render extent, anything else is used literally. */
        .mvecScaleX = fgMvecScaleX * (float)fgMvecSign,
        .mvecScaleY = fgMvecScaleY * (float)fgMvecSign,

        .cameraNear = vkpt_refdef.z_near,
        .cameraFar = vkpt_refdef.z_far,
        .cameraFOV = fovYRadians,
        .cameraAspectRatio = aspect,

        /* SAME SIGN CONVENTION AS SUPER RESOLUTION. DLSSApply() multiplies the jitter
           by pt_dlss_jitter_sign (default -1) before handing it over, because Q2RTX never
           jitters a projection matrix - primary_rays.rgen samples at
           (pixel_centre + sub_pixel_jitter), and sampling toward +j puts scene content at
           -j, so the equivalent projection jitter is negated. This path was passing the
           RAW offset, i.e. the opposite sign to the one super resolution uses.

           A wrong-signed jitter displaces content by a sub-pixel amount that flips every
           frame. Invisible when the camera is still; reads as judder as soon as it turns,
           which is exactly how it showed up. */
        .jitterOffsetX = ubo->sub_pixel_jitter[0] * fgJitterSign,
        .jitterOffsetY = ubo->sub_pixel_jitter[1] * fgJitterSign,

        .pCameraViewToClip = mViewToClip,
        .pClipToCameraView = mClipToView,
        .pClipToPrevClip = mClipToPrev,
        .pPrevClipToClip = mPrevToClip,

        /* VKPT_IMG_DLSS_OUTPUT is linear HDR - tone mapping happens after this point. */
        /* NOT "is our colour buffer linear HDR" - the SDK comment on this field says
           "full HDR (RENDERING TO AN HDR MONITOR)". It describes the OUTPUT path, not the
           format of the buffer we hand over, and we were passing 1 on an SDR swapchain.

           The cost was visible and constant: dumped side by side, the generated frame had
           the same geometry and view as the real one but its highlights were crushed - a
           blown-out white window came back dull amber, the strip light came back orange -
           while the mid-tones lifted (mean 86 vs 62 in sRGB). Every generated frame
           therefore looked wrong in any scene with a bright light in it, which at 2x is
           every other frame presented, and reads as constant artifacting.

           Note the SR path a few hundred lines up passes NVSDK_NGX_DLSS_Feature_Flags_IsHDR
           unconditionally and that is CORRECT - for Super Resolution the flag means the
           INPUT is HDR linear. Same word, different question. Do not "make them consistent". */
        .colorBuffersHDR = qvk.surf_is_hdr ? 1 : 0,

        /* VKPT_IMG_DLSS_DEPTH is LINEAR view depth, not a reversed hardware depth
           buffer - the SR/RR features are created with
           NVSDK_NGX_DLSS_Depth_Type_Linear. DLSS-G has no equivalent "depth type"
           parameter, so if the interpolation misbehaves around depth discontinuities
           this is a prime suspect. */
        /* A/B KNOBS FOR THE INPUT CONVENTIONS.

           The compare instrument measures the generated frame sitting ~100% of a frame
           step from the real one when it should be ~50%, i.e. on top of an endpoint
           rather than between the two. Neither split fields nor mvecScale changed it, so
           what remains is a convention mismatch in what we hand DLSS-G. These are all
           per-frame Opt_Eval fields, so they take effect immediately - no feature
           recreation, no rebuild, sweepable in one session.

             pt_dlss_fg_mvsign    -1 flips the motion vector direction. The guide says the
                                  buffer must describe motion from the CURRENT frame to the
                                  PREVIOUS one; Q2RTX's DLSS motion may run the other way,
                                  and a sign error moves everything backwards, which
                                  interpolation resolves toward an endpoint.
             pt_dlss_fg_cammotion 0 says camera motion is NOT already baked into the mvecs,
                                  so DLSS-G derives it from the matrices instead.
             pt_dlss_fg_depthinv  1 for reversed-Z depth. VKPT_IMG_DLSS_DEPTH is linear view
                                  depth, so 0 should be right - but it has never been tested
                                  against 1, and getting it wrong breaks disocclusion. */
        .depthInverted = Cvar_Get("pt_dlss_fg_depthinv", "0", 0)->integer ? 1 : 0,

        .cameraMotionIncluded = Cvar_Get("pt_dlss_fg_cammotion", "1", 0)->integer ? 1 : 0,
        .reset = resetAccum ? 1 : 0,
        .notRenderingGameFrames = qvk.frame_menu_mode ? 1 : 0,

        .multiFrameCount = generatedFrames,
        .multiFrameIndex = genIndex,
    };

    unsigned int res = DLSSG_EvaluateFeature(cmd, dlssgFeature, dlssObj.pParams, &in);

    if (res != (unsigned int)NVSDK_NGX_Result_Success) {
        Com_EPrintf("DLSS-G: evaluate failed on generated frame %u/%u: 0x%08x"
                    " - see DLSSTemp/nvngx_dlssg*.log\n", genIndex, generatedFrames, res);
        /* Evaluate failures are usually structural (bad resource, wrong size), not
           transient, so stop rather than repeat the message every frame. */
        dlssgFailed = qtrue;
        return;
    }
    }
}
