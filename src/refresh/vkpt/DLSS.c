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

DLSS dlssObj;
cvar_t* cvar_pt_dlss = NULL;
cvar_t* cvar_pt_dlss_dldn = NULL;
qboolean recreateSwapChain = qfalse;
qboolean dlssModeChanged = qfalse;
extern cvar_t* scr_viewsize;
int oldCvarValue;

void InitDLSSCvars() 
{
    cvar_pt_dlss = Cvar_Get("pt_dlss", "0", CVAR_ARCHIVE);
    cvar_pt_dlss_dldn = Cvar_Get("pt_dlss_dldn", "0", CVAR_ARCHIVE);
    oldCvarValue = cvar_pt_dlss->integer;
    cvar_pt_dlss->changed = viewsize_changed;
    cvar_pt_dlss_dldn->changed = DlssModeChanged;
    viewsize_changed(cvar_pt_dlss);
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
            return .77f;
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
        return (4 * .77f);
    case 5:
        return 1;
    default:
        return 1.0;
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
        vkDeviceWaitIdle(dlssObj.device);

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

    vkDeviceWaitIdle(dlssObj.device);

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
        myValue = NVSDK_NGX_PerfQuality_Value_UltraQuality;
        break;
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

    SaveDLSSFeatureValues(resObject);

    if (dlssObj.pDlssFeature != NULL) {
        DestroyDLSSFeature();
    }


    NVSDK_NGX_DLDenoise_Create_Params denoiseParm = {
         .Feature = {.InWidth = resObject.inputWidth,
                     .InHeight = resObject.inputHeight,
                     .InTargetWidth = resObject.outputWidth,
                     .InTargetHeight = resObject.outputHeight,
                     .InPerfQualityValue = ToNGXPerfQuality()
                   }
        
    };

    NVSDK_NGX_DLSS_Create_Params dlssParams = {
        .Feature = {.InWidth = resObject.inputWidth,
                     .InHeight = resObject.inputHeight,
                     .InTargetWidth = resObject.outputWidth,
                     .InTargetHeight = resObject.outputHeight,
                     .InPerfQualityValue = ToNGXPerfQuality()
                   }
    };

    int DlssCreateFeatureFlags = NVSDK_NGX_DLSS_Feature_Flags_None;
    
    //DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
   
    DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_Reserved_0;
    DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    DlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;


    dlssParams.InFeatureCreateFlags = DlssCreateFeatureFlags;
    denoiseParm.InFeatureCreateFlags = DlssCreateFeatureFlags;

    // only one phys device
    uint32_t creationNodeMask = 1;
    uint32_t visibilityNodeMask = 1;    
    bool denoiseMode = Cvar_Get("pt_dlss_dldn", "0", CVAR_ARCHIVE)->integer == 1;;

    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_DLSS_Hint_Render_Preset_J);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_DLSS_Hint_Render_Preset_J);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_J);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, NVSDK_NGX_DLSS_Hint_Render_Preset_J);
    
    NVSDK_NGX_Parameter_SetF(dlssObj.pParams, NVSDK_NGX_Parameter_Hint_UseFireflySwatter, 1.0f);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_Denoise, 1);
    NVSDK_NGX_Parameter_SetUI(dlssObj.pParams, NVSDK_NGX_Parameter_DenoiseMode, 1);
    NVSDK_NGX_Result res;
    NVSDK_NGX_Result ssres;

    if (!denoiseMode) {
        res = NGX_VULKAN_CREATE_DLSS_EXT(cmd, creationNodeMask, visibilityNodeMask, &dlssObj.pDlssFeature, dlssObj.pParams, &dlssParams);
        ssres = NVSDK_NGX_VULKAN_CreateFeature(cmd, NVSDK_NGX_Feature_SuperSampling, dlssObj.pParams, &dlssObj.pDlssFeature);
    }
    else {
        res = NGX_VULKAN_CREATE_DLSSDN_EXT(cmd, creationNodeMask, visibilityNodeMask, &dlssObj.pDlssFeature, dlssObj.pParams, &denoiseParm);
        ssres = NVSDK_NGX_VULKAN_CreateFeature(cmd, NVSDK_NGX_Feature_Reserved13, dlssObj.pParams, &dlssObj.pDlssFeature);
    }
  
    if (NVSDK_NGX_FAILED(res))
    {
        Com_EPrintf("DLSS: NGX_VULKAN_CREATE_DLSS_EXT fail: %d", (int)res);

        dlssObj.pDlssFeature = NULL;
        return qfalse;
    }

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
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_TRANSPARENT]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_PT_MOTION]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_PT_REFLECT_MOTION]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_ALBEDO]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_SPECULAR]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_ROUGHNESS]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_METALLIC]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_NORMAL]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_MATERIALID]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_EMISSIVE]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_INDIRECT_ALBEDO]);
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
    NVSDK_NGX_Resource_VK depthResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_DEPTH], qvk.images_views[VKPT_IMG_DLSS_DEPTH], targetSize, VK_FORMAT_R32G32B32A32_SFLOAT, false);
    NVSDK_NGX_Resource_VK rayLengthResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_RAY_LENGTH], qvk.images_views[VKPT_IMG_DLSS_RAY_LENGTH], sourceSize, VK_FORMAT_R32G32B32A32_SFLOAT, false);
    NVSDK_NGX_Resource_VK transparentResoruce = ToNGXResource(qvk.images[VKPT_IMG_DLSS_TRANSPARENT], qvk.images_views[VKPT_IMG_DLSS_TRANSPARENT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK motionVec3D = ToNGXResource(qvk.images[VKPT_IMG_DLSS_3DMOTION_VECTOR], qvk.images_views[VKPT_IMG_DLSS_3DMOTION_VECTOR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK reflectMotion = ToNGXResource(qvk.images[VKPT_IMG_DLSS_REFLECT_MOTION], qvk.images_views[VKPT_IMG_DLSS_REFLECT_MOTION], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK albedo = ToNGXResource(qvk.images[VKPT_IMG_DLSS_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK specular = ToNGXResource(qvk.images[VKPT_IMG_DLSS_SPECULAR], qvk.images_views[VKPT_IMG_DLSS_SPECULAR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK roughness = ToNGXResource(qvk.images[VKPT_IMG_DLSS_ROUGHNESS], qvk.images_views[VKPT_IMG_DLSS_ROUGHNESS], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK metallic = ToNGXResource(qvk.images[VKPT_IMG_DLSS_METALLIC], qvk.images_views[VKPT_IMG_DLSS_METALLIC], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK normal = ToNGXResource(qvk.images[VKPT_IMG_DLSS_NORMAL], qvk.images_views[VKPT_IMG_DLSS_NORMAL], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK materialid = ToNGXResource(qvk.images[VKPT_IMG_DLSS_MATERIALID], qvk.images_views[VKPT_IMG_DLSS_MATERIALID], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK emissive = ToNGXResource(qvk.images[VKPT_IMG_DLSS_EMISSIVE], qvk.images_views[VKPT_IMG_DLSS_EMISSIVE], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK indirectAlbedo = ToNGXResource(qvk.images[VKPT_IMG_DLSS_INDIRECT_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_INDIRECT_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK specularAlbedo = ToNGXResource(qvk.images[VKPT_IMG_DLSS_SPECULAR_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_SPECULAR_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK beforeTransparent = ToNGXResource(qvk.images[VKPT_IMG_DLSS_BEFORE_TRANSPARENT], qvk.images_views[VKPT_IMG_DLSS_BEFORE_TRANSPARENT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK diffuseLength = ToNGXResource(qvk.images[VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE], qvk.images_views[VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK specularLength = ToNGXResource(qvk.images[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR], qvk.images_views[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK reflectedAlbedo = ToNGXResource(qvk.images[VKPT_IMG_DLSS_REFLECTED_ALBEDO], qvk.images_views[VKPT_IMG_DLSS_REFLECTED_ALBEDO], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);

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
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_REFLECT_MOTION], qvk.images_views[VKPT_IMG_DLSS_REFLECT_MOTION], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
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
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_TRANSPARENT], qvk.images_views[VKPT_IMG_DLSS_TRANSPARENT], sourceSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
            break;
        case 14:
            unresolvedColorResource = ToNGXResource(qvk.images[VKPT_IMG_DLSS_DEPTH], qvk.images_views[VKPT_IMG_DLSS_DEPTH], targetSize, VK_FORMAT_R32G32B32A32_SFLOAT, false);
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


    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_SPECULAR] = &specular;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_ROUGHNESS] = &roughness;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_METALLIC] = &metallic;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_NORMALS] = &normal;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_MATERIALID] = &materialid;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_EMISSIVE] = &emissive;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_INDIRECT_ALBEDO] = &indirectAlbedo;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_SPECULAR_ALBEDO] = &specularAlbedo;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_BEFORE_PARTICLES] = &beforeTransparent;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_BEFORE_TRANSPARENCY] = &beforeTransparent;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_DIFFUSE_HITDISTANCE] = &diffuseLength;
    //inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_SPECULAR_HITDISTANCE] = &specularLength;
    inBuffer.pInAttrib[NVSDK_NGX_GBUFFER_REFLECTED_ALBEDO] = &reflectedAlbedo;

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {
        .Feature = {.pInColor = &unresolvedColorResource, .pInOutput = &resolvedColorResource },
        .pInDepth = &depthResource,
        .pInMotionVectors = &motionVectorsResource,
        .InJitterOffsetX = jitterOffset[0] * (-1),
        .InJitterOffsetY = jitterOffset[1] * (-1),
        .InRenderSubrectDimensions = sourceSize,
        .InReset = resetAccum ? 1 : 0,
        .InMVScaleX = sourceSize.Width,
        .InMVScaleY = sourceSize.Height,
        .InColorSubrectBase = sourceOffset,
        .InDepthSubrectBase = sourceOffset,
        .InMVSubrectBase = sourceOffset,
        .InTranslucencySubrectBase = sourceOffset,
        .InFrameTimeDeltaInMsec = timeDelta * 1000.0,
        .pInRayTracingHitDistance = &rayLengthResource,
        .pInMotionVectors3D = &motionVec3D,
        .pInTransparencyMask = &transparentResoruce,
        .pInMotionVectorsReflections = &reflectMotion,
        .InToneMapperType = NVSDK_NGX_TONEMAPPER_REINHARD,
        .GBufferSurface = inBuffer,
        
    };    

    NVSDK_NGX_Result res = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, dlssObj.pDlssFeature, dlssObj.pParams, &evalParams);

    if (NVSDK_NGX_FAILED(res))
    {
        Com_EPrintf("DLSS: NGX_VULKAN_EVALUATE_DLSS_EXT fail: %d", (int)res);
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
        Cvar_SetInteger(scr_viewsize, 77, FROM_MENU);
        return;
    case 5:
        Cvar_SetInteger(scr_viewsize, 100, FROM_MENU);
        return;
    }

    oldCvarValue = self->integer;
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
        Cvar_SetInteger(scr_viewsize, 77, FROM_MENU);
        return;
    case 5:
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


