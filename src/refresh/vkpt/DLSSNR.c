/*
DLSS 5 NEURAL RENDERING - engine side. See DLSSNR.h and dlssnr_forwarder.cpp.

WHAT THE MODEL IS FED

  Color   VKPT_IMG_DLSS_OUTPUT at output resolution, tone mapped and HUD-less, ENCODED TO
          sRGB first by dlss5_convert.comp mode 0. The frame is linear until the swapchain
          blit encodes it, and the model expects what an SDR game buffer holds - sRGB. Feeding
          it linear made its colour adaptation hunt between red and washed-out blue.
  Depth   VKPT_IMG_DLSS_DEPTH, render resolution, not inverted - same image DLSS gets.
  MVec    VKPT_IMG_PT_DLSS_MOTION, render resolution, UV units, so the scale is the render
          size - again exactly what DLSS gets (InMVScaleX = render width).
  Output  VKPT_IMG_MOTION_BLUR. That is scratch for the motion blur pass, which runs before
          tone mapping and copies its result out, so it is free here; it is rgba16f and at
          least output-sized. Using a global image lets the resolve shader read the model's
          answer and the original side by side (mode 1), which is what makes the strength,
          colour and difference controls possible.

TUNING IS READ AT CREATE TIME. The model reads preset, style and the strengths once, when the
feature is built, and ignores them at evaluate (OptiScaler found this the hard way). So every
tuning cvar marks the feature dirty, and it is rebuilt once the value has held still for
DLSS5_REBUILD_DELAY_MS - dragging a slider would otherwise rebuild it every frame. They are
also re-sent at evaluate, which is harmless and covers any the model does read live.
pt_dlss5_strength, _colour and _compare are ours, applied in the resolve, and are live.
*/

#include "vkpt.h"
#include "DLSS.h"
#include "DLSSNR.h"
#include "system/system.h"

#include <windows.h>

#define DLSS5_REBUILD_DELAY_MS 300

typedef int   (*PFN_dlssnr_load)(const wchar_t*);
typedef int   (*PFN_dlssnr_init)(const wchar_t*, void*, void*, void*);
typedef int   (*PFN_dlssnr_shutdown)(void*);
typedef void* (*PFN_dlssnr_params_create)(void);
typedef void  (*PFN_dlssnr_params_destroy)(void*);
typedef void  (*PFN_dlssnr_set_ui)(void*, const char*, unsigned int);
typedef void  (*PFN_dlssnr_set_f)(void*, const char*, float);
typedef void  (*PFN_dlssnr_set_ptr)(void*, const char*, void*);
typedef int   (*PFN_dlssnr_missed_keys)(void*, char*, int);
typedef int   (*PFN_dlssnr_create)(void*, void*, void**);
typedef int   (*PFN_dlssnr_evaluate)(void*, void*, void*);
typedef int   (*PFN_dlssnr_release)(void*);

static struct {
    HMODULE forwarder;
    PFN_dlssnr_load load;
    PFN_dlssnr_init init;
    PFN_dlssnr_shutdown shutdown;
    PFN_dlssnr_params_create params_create;
    PFN_dlssnr_params_destroy params_destroy;
    PFN_dlssnr_set_ui set_ui;
    PFN_dlssnr_set_f set_f;
    PFN_dlssnr_set_ptr set_ptr;
    PFN_dlssnr_missed_keys missed_keys;
    PFN_dlssnr_create create;
    PFN_dlssnr_evaluate evaluate;
    PFN_dlssnr_release release;

    qboolean failed;            /* sticky until pt_dlss5 is toggled */
    char reason[256];
    int lastResult;

    qboolean ngxInitialised;
    VkDevice ngxDevice;
    void* params;
    void* feature;
    uint32_t width, height;     /* what the feature was built for */

    qboolean reset;
    qboolean dirty;
    unsigned dirtyTime;
    unsigned long long frames;
    qboolean warnedHdr;
} nr;

static VkPipeline       convert_pipeline;
static VkPipelineLayout convert_pipeline_layout;

struct dlss5_convert_push_constants {
    int32_t extent[2];
    int32_t mode;       /* 0 encode, 1 resolve */
    int32_t split;
    float   strength;
    float   colour;
    int32_t view;
};

static cvar_t* cvar_pt_dlss5;
static cvar_t* cvar_pt_dlss5_preset;
static cvar_t* cvar_pt_dlss5_style;
static cvar_t* cvar_pt_dlss5_intensity;
static cvar_t* cvar_pt_dlss5_local_structure;
static cvar_t* cvar_pt_dlss5_local_tone;
static cvar_t* cvar_pt_dlss5_skin;
static cvar_t* cvar_pt_dlss5_automask;
static cvar_t* cvar_pt_dlss5_ui_correction;
static cvar_t* cvar_pt_dlss5_compare;
static cvar_t* cvar_pt_dlss5_strength;
static cvar_t* cvar_pt_dlss5_colour;

VkResult vkpt_dlss5_initialize(void)
{
    VkDescriptorSetLayout desc_set_layouts[] = {
        qvk.desc_set_layout_ubo, qvk.desc_set_layout_textures,
    };
    VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(struct dlss5_convert_push_constants)
    };
    CREATE_PIPELINE_LAYOUT(qvk.device, &convert_pipeline_layout,
        .setLayoutCount         = LENGTH(desc_set_layouts),
        .pSetLayouts            = desc_set_layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_constant_range
    );
    ATTACH_LABEL_VARIABLE(convert_pipeline_layout, PIPELINE_LAYOUT);
    return VK_SUCCESS;
}

VkResult vkpt_dlss5_destroy(void)
{
    vkDestroyPipelineLayout(qvk.device, convert_pipeline_layout, NULL);
    convert_pipeline_layout = NULL;
    return VK_SUCCESS;
}

VkResult vkpt_dlss5_create_pipelines(void)
{
    VkComputePipelineCreateInfo pipeline_info = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = SHADER_STAGE(QVK_MOD_DLSS5_CONVERT_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
        .layout = convert_pipeline_layout,
    };
    _VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &convert_pipeline));
    ATTACH_LABEL_VARIABLE(convert_pipeline, PIPELINE);
    return VK_SUCCESS;
}

VkResult vkpt_dlss5_destroy_pipelines(void)
{
    vkDestroyPipeline(qvk.device, convert_pipeline, NULL);
    convert_pipeline = NULL;
    return VK_SUCCESS;
}

static void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags src, VkAccessFlags dst)
{
    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    IMAGE_BARRIER(cmd,
        .image = image,
        .subresourceRange = range,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL);
}

/* Writes VKPT_IMG_DLSS_OUTPUT, then a barrier so whatever reads it next - NGX, frame
   generation, the final blit - sees the result. */
static void Convert(VkCommandBuffer cmd, int mode, uint32_t width, uint32_t height)
{
    VkDescriptorSet desc_sets[] = {
        qvk.desc_set_ubo,
        qvk_get_current_desc_set_textures(),
    };
    const int compare = cvar_pt_dlss5_compare->integer;
    struct dlss5_convert_push_constants push = {
        .extent = { (int32_t)width, (int32_t)height },
        .mode = mode,
        .split = compare == 1 ? (int32_t)(width / 2) : 0,
        .strength = cvar_pt_dlss5_strength->value,
        .colour = cvar_pt_dlss5_colour->value,
        .view = compare,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, convert_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        convert_pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, 0);
    vkCmdPushConstants(cmd, convert_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(push), &push);
    vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);

    ImageBarrier(cmd, qvk.images[VKPT_IMG_DLSS_OUTPUT], VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
}

static void Fail(const char* why, int result)
{
    if (nr.failed)
        return;
    nr.failed = qtrue;
    nr.lastResult = result;
    Q_strlcpy(nr.reason, why, sizeof(nr.reason));
    Com_EPrintf("DLSS 5: %s (result 0x%08X). Switch pt_dlss5 off and on to retry; dlss5_info for details.\n",
        why, (unsigned)result);
}

static void ReleaseFeature(void)
{
    if (nr.feature && nr.release)
        nr.release(nr.feature);
    nr.feature = NULL;
    nr.width = nr.height = 0;
    nr.reset = qtrue;
}

void DLSS5_DestroyFeature(void)
{
    ReleaseFeature();
}

void DLSS5_Shutdown(void)
{
    ReleaseFeature();

    if (nr.params && nr.params_destroy)
        nr.params_destroy(nr.params);
    nr.params = NULL;

    if (nr.ngxInitialised && nr.shutdown)
        nr.shutdown(nr.ngxDevice);
    nr.ngxInitialised = qfalse;
    nr.ngxDevice = VK_NULL_HANDLE;

    Cmd_RemoveCommand("dlss5_info");

    /* The forwarder and the snippet stay loaded: unloading a library NGX has threads or
       callbacks registered from is not worth the risk for a vid_restart. */
}

static void tuning_changed(cvar_t* self)
{
    (void)self;
    nr.dirty = qtrue;
    nr.dirtyTime = Sys_Milliseconds();
}

static void enable_changed(cvar_t* self)
{
    /* Toggling is the retry. Turning it off frees the feature's VRAM - the model is large. */
    nr.failed = qfalse;
    nr.reason[0] = 0;
    if (self->integer == 0 && nr.feature) {
        vkpt_device_wait_idle();
        ReleaseFeature();
    }
}

static void dlss5_info_f(void)
{
    Com_Printf("DLSS 5 Neural Rendering\n");
    Com_Printf("  pt_dlss5 %d, DLSS %s\n", cvar_pt_dlss5->integer, DLSSEnabled() ? "on" : "OFF (required)");
    Com_Printf("  forwarder %s, NGX %s, feature %s %ux%u, frames %llu\n",
        nr.forwarder ? "loaded" : "not loaded",
        nr.ngxInitialised ? "initialised" : "not initialised",
        nr.feature ? "up" : "down", nr.width, nr.height, nr.frames);
    Com_Printf("  preset %d, style %d, intensity %.2f, local structure %.2f, local tone %.2f, skin %.2f, auto mask %d\n",
        cvar_pt_dlss5_preset->integer, cvar_pt_dlss5_style->integer, cvar_pt_dlss5_intensity->value,
        cvar_pt_dlss5_local_structure->value, cvar_pt_dlss5_local_tone->value, cvar_pt_dlss5_skin->value,
        cvar_pt_dlss5_automask->integer);
    Com_Printf("  strength %.2f, colour %.2f, compare %d\n",
        cvar_pt_dlss5_strength->value, cvar_pt_dlss5_colour->value, cvar_pt_dlss5_compare->integer);
    if (nr.failed)
        Com_Printf("  FAILED: %s (result 0x%08X)\n", nr.reason, (unsigned)nr.lastResult);

    if (nr.params && nr.missed_keys) {
        static char buf[8192];
        int n = nr.missed_keys(nr.params, buf, sizeof(buf));
        Com_Printf("  %d parameter(s) the model asked for that were never set:\n%s", n, buf);
    }
}

void DLSS5_InitCvars(void)
{
    cvar_pt_dlss5 = Cvar_Get("pt_dlss5", "0", CVAR_ARCHIVE);
    cvar_pt_dlss5->changed = enable_changed;

    /* NR's own preset scale: 0 lets the model choose, 1-3 are its alternatives. NOT the
       Super Resolution letters - J there is 10, which this model does not have. */
    cvar_pt_dlss5_preset = Cvar_Get("pt_dlss5_preset", "0", CVAR_ARCHIVE);
    /* 0 standard, 1 natural, 2 cinematic */
    cvar_pt_dlss5_style = Cvar_Get("pt_dlss5_style", "0", CVAR_ARCHIVE);
    cvar_pt_dlss5_intensity = Cvar_Get("pt_dlss5_intensity", "1.0", CVAR_ARCHIVE);
    cvar_pt_dlss5_local_structure = Cvar_Get("pt_dlss5_local_structure", "1.0", CVAR_ARCHIVE);
    cvar_pt_dlss5_local_tone = Cvar_Get("pt_dlss5_local_tone", "1.0", CVAR_ARCHIVE);
    /* -1 leaves it to the model, which is also OptiScaler's default */
    cvar_pt_dlss5_skin = Cvar_Get("pt_dlss5_skin", "-1.0", CVAR_ARCHIVE);
    cvar_pt_dlss5_automask = Cvar_Get("pt_dlss5_automask", "1", CVAR_ARCHIVE);
    /* The model can correct around a UI layer it is given. We run before the HUD is drawn,
       so there is none; 0 by default, exposed in case the model behaves better with it. */
    cvar_pt_dlss5_ui_correction = Cvar_Get("pt_dlss5_ui_correction", "0", CVAR_ARCHIVE);

    /* Ours, applied in the resolve, live. */
    /* 0 off, 1 split screen (left half is the frame without DLSS 5), 2 difference x8 */
    cvar_pt_dlss5_compare = Cvar_Get("pt_dlss5_compare", "0", 0);
    /* 0 original, 1 the model, above 1 exaggerates the model's edit */
    cvar_pt_dlss5_strength = Cvar_Get("pt_dlss5_strength", "1.0", CVAR_ARCHIVE);
    /* 0 keeps the original hue (luminance change only), 1 the model's colour too.
       Default 0: on Quake 2 most of what the model does with colour is a global re-grade
       it keeps re-deciding - teal/desaturated one moment, saturated red the next - and it
       invents hues outright (a yellow ammo readout goes magenta). Its lighting and detail
       edits survive at 0; that is the part worth having. */
    cvar_pt_dlss5_colour = Cvar_Get("pt_dlss5_colour", "0", CVAR_ARCHIVE);

    cvar_pt_dlss5_preset->changed = tuning_changed;
    cvar_pt_dlss5_style->changed = tuning_changed;
    cvar_pt_dlss5_intensity->changed = tuning_changed;
    cvar_pt_dlss5_local_structure->changed = tuning_changed;
    cvar_pt_dlss5_local_tone->changed = tuning_changed;
    cvar_pt_dlss5_skin->changed = tuning_changed;
    cvar_pt_dlss5_automask->changed = tuning_changed;
    cvar_pt_dlss5_ui_correction->changed = tuning_changed;

    Cmd_AddCommand("dlss5_info", dlss5_info_f);
}

qboolean DLSS5Enabled(void)
{
    return cvar_pt_dlss5 && cvar_pt_dlss5->integer != 0 && !nr.failed && qvk.supports_ngx;
}

/* Module directory of q2rtx.exe, with a trailing backslash. */
static void ExeDir(wchar_t* out, size_t size)
{
    GetModuleFileNameW(NULL, out, (DWORD)size);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash)
        slash[1] = 0;
}

static qboolean EnsureLoaded(void)
{
    if (nr.forwarder)
        return qtrue;

    wchar_t dir[MAX_PATH], path[MAX_PATH];
    ExeDir(dir, MAX_PATH);

    swprintf(path, MAX_PATH, L"%lsnvngx.dll_dlssnr.dll", dir);
    HMODULE m = LoadLibraryW(path);
    if (!m) {
        Fail("nvngx.dll_dlssnr.dll (the forwarder) was not found next to q2rtx.exe", 0);
        return qfalse;
    }

#define GET(field, name) nr.field = (PFN_##name)GetProcAddress(m, #name)
    GET(load, dlssnr_load);
    GET(init, dlssnr_init);
    GET(shutdown, dlssnr_shutdown);
    GET(params_create, dlssnr_params_create);
    GET(params_destroy, dlssnr_params_destroy);
    GET(set_ui, dlssnr_set_ui);
    GET(set_f, dlssnr_set_f);
    GET(set_ptr, dlssnr_set_ptr);
    GET(missed_keys, dlssnr_missed_keys);
    GET(create, dlssnr_create);
    GET(evaluate, dlssnr_evaluate);
    GET(release, dlssnr_release);
#undef GET

    if (!nr.load || !nr.init || !nr.params_create || !nr.set_ui || !nr.set_f || !nr.set_ptr
        || !nr.create || !nr.evaluate || !nr.release) {
        Fail("the forwarder is missing exports - rebuild nvngx.dll_dlssnr.dll", 0);
        return qfalse;
    }
    nr.forwarder = m;

    swprintf(path, MAX_PATH, L"%lsnvngx_dlssnr.dll", dir);
    int bits = nr.load(path);
    if ((bits & 15) != 15) {
        Fail(bits == 0 ? "nvngx_dlssnr.dll was not found next to q2rtx.exe"
                       : "nvngx_dlssnr.dll is missing Vulkan entry points", bits);
        return qfalse;
    }
    return qtrue;
}

static qboolean EnsureNGX(void)
{
    if (nr.ngxInitialised && nr.ngxDevice == qvk.device)
        return qtrue;

    int r = nr.init(L"DLSSTemp/", qvk.instance, qvk.physical_device, qvk.device);
    if (r != NVSDK_NGX_Result_Success) {
        Fail("NVSDK_NGX_VULKAN_Init_Ext on nvngx_dlssnr.dll failed", r);
        return qfalse;
    }
    nr.ngxInitialised = qtrue;
    nr.ngxDevice = qvk.device;

    if (!nr.params)
        nr.params = nr.params_create();
    Com_Printf("DLSS 5: NGX initialised on nvngx_dlssnr.dll\n");
    return qtrue;
}

static void SetTuning(void)
{
    void* p = nr.params;
    const int preset = cvar_pt_dlss5_preset->integer;
    const int style = cvar_pt_dlss5_style->integer;
    nr.set_ui(p, "DLSSNR.Hint.Render.Preset", (unsigned)(preset < 0 ? 0 : preset));
    nr.set_f(p, "DLSSNR.Intensity", cvar_pt_dlss5_intensity->value);
    nr.set_ui(p, "DLSSNR.Style", (unsigned)(style < 0 ? 0 : style));
    nr.set_f(p, "DLSSNR.LocalStructureStrength", cvar_pt_dlss5_local_structure->value);
    nr.set_f(p, "DLSSNR.LocalToneStrength", cvar_pt_dlss5_local_tone->value);
    nr.set_f(p, "DLSSNR.SkinStructureStrength", cvar_pt_dlss5_skin->value);
    nr.set_ui(p, "DLSSNR.UseAutoMask", cvar_pt_dlss5_automask->integer ? 1u : 0u);
}

static qboolean EnsureFeature(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    qboolean rebuild = nr.feature && nr.dirty && Sys_Milliseconds() - nr.dirtyTime >= DLSS5_REBUILD_DELAY_MS;
    if (nr.feature && (nr.width != width || nr.height != height))
        rebuild = qtrue;
    if (rebuild) {
        /* Frames in flight still reference the feature. */
        vkpt_device_wait_idle();
        ReleaseFeature();
    }
    if (nr.feature)
        return qtrue;

    void* p = nr.params;
    nr.set_ui(p, "DLSSNR.Enabled", 1u);
    nr.set_ui(p, "DLSSNR.Width", width);
    nr.set_ui(p, "DLSSNR.Height", height);
    nr.set_ui(p, "CreationNodeMask", 1u);
    nr.set_ui(p, "VisibilityNodeMask", 1u);
    nr.set_ui(p, "DLSSNR.UICorrection", cvar_pt_dlss5_ui_correction->integer ? 1u : 0u);
    SetTuning();

    void* handle = NULL;
    int r = nr.create(cmd, p, &handle);
    if (r != NVSDK_NGX_Result_Success || !handle) {
        Fail("CreateFeature(18) failed", r);
        return qfalse;
    }
    nr.feature = handle;
    nr.width = width;
    nr.height = height;
    nr.dirty = qfalse;
    nr.reset = qtrue;
    Com_Printf("DLSS 5: feature created at %ux%u (preset %d, style %d, intensity %.2f)\n",
        width, height, cvar_pt_dlss5_preset->integer, cvar_pt_dlss5_style->integer,
        cvar_pt_dlss5_intensity->value);
    return qtrue;
}

void DLSS5Apply(VkCommandBuffer cmd, qboolean resetHistory)
{
    if (!DLSS5Enabled())
        return;

    if (!nr.warnedHdr && Cvar_VariableInteger("vid_hdr")) {
        nr.warnedHdr = qtrue;
        Com_WPrintf("DLSS 5: vid_hdr is on. The model is built for an SDR, display-referred frame, "
            "so expect it to misjudge brightness.\n");
    }

    const uint32_t width = qvk.extent_unscaled.width;
    const uint32_t height = qvk.extent_unscaled.height;
    const uint32_t guideWidth = qvk.extent_render.width;
    const uint32_t guideHeight = qvk.extent_render.height;

    if (!EnsureLoaded() || !EnsureNGX() || !EnsureFeature(cmd, width, height))
        return;

    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_DEPTH]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_PT_DLSS_MOTION]);
    /* Motion blur's scratch: last written/read by that pass (compute + transfer). */
    ImageBarrier(cmd, qvk.images[VKPT_IMG_MOTION_BLUR],
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_WRITE_BIT);

    /* The model wants sRGB-encoded values; the frame is linear. */
    Convert(cmd, 0, width, height);

    NVSDK_NGX_Dimensions outSize = { width, height };
    NVSDK_NGX_Dimensions guideSize = { guideWidth, guideHeight };

    NVSDK_NGX_Resource_VK color = ToNGXResource(qvk.images[VKPT_IMG_DLSS_OUTPUT], qvk.images_views[VKPT_IMG_DLSS_OUTPUT],
        outSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK depth = ToNGXResource(qvk.images[VKPT_IMG_DLSS_DEPTH], qvk.images_views[VKPT_IMG_DLSS_DEPTH],
        guideSize, VK_FORMAT_R32_SFLOAT, false);
    NVSDK_NGX_Resource_VK motion = ToNGXResource(qvk.images[VKPT_IMG_PT_DLSS_MOTION], qvk.images_views[VKPT_IMG_PT_DLSS_MOTION],
        guideSize, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    NVSDK_NGX_Resource_VK output = ToNGXResource(qvk.images[VKPT_IMG_MOTION_BLUR], qvk.images_views[VKPT_IMG_MOTION_BLUR],
        outSize, VK_FORMAT_R16G16B16A16_SFLOAT, true);

    void* p = nr.params;
    nr.set_ptr(p, "DLSSNR.Color", &color);
    nr.set_ptr(p, "DLSSNR.Depth", &depth);
    nr.set_ptr(p, "DLSSNR.MVec", &motion);
    nr.set_ptr(p, "DLSSNR.Output", &output);

    nr.set_ui(p, "DLSSNR.Enabled", 1u);
    nr.set_ui(p, "DLSSNR.Width", width);
    nr.set_ui(p, "DLSSNR.Height", height);
    nr.set_ui(p, "DLSSNR.DepthInverted", 0u);
    nr.set_ui(p, "DLSSNR.Reset", (resetHistory || nr.reset) ? 1u : 0u);

    nr.set_ui(p, "DLSSNR.ColorSubrectBaseX", 0u);
    nr.set_ui(p, "DLSSNR.ColorSubrectBaseY", 0u);
    nr.set_ui(p, "DLSSNR.ColorSubrectWidth", width);
    nr.set_ui(p, "DLSSNR.ColorSubrectHeight", height);
    nr.set_ui(p, "DLSSNR.OutputSubrectBaseX", 0u);
    nr.set_ui(p, "DLSSNR.OutputSubrectBaseY", 0u);
    nr.set_ui(p, "DLSSNR.OutputSubrectWidth", width);
    nr.set_ui(p, "DLSSNR.OutputSubrectHeight", height);
    nr.set_ui(p, "DLSSNR.DepthSubrectBaseX", 0u);
    nr.set_ui(p, "DLSSNR.DepthSubrectBaseY", 0u);
    nr.set_ui(p, "DLSSNR.DepthSubrectWidth", guideWidth);
    nr.set_ui(p, "DLSSNR.DepthSubrectHeight", guideHeight);
    nr.set_ui(p, "DLSSNR.MVecSubrectBaseX", 0u);
    nr.set_ui(p, "DLSSNR.MVecSubrectBaseY", 0u);
    nr.set_ui(p, "DLSSNR.MVecSubrectWidth", guideWidth);
    nr.set_ui(p, "DLSSNR.MVecSubrectHeight", guideHeight);
    nr.set_f(p, "DLSSNR.MVecScaleX", (float)guideWidth);
    nr.set_f(p, "DLSSNR.MVecScaleY", (float)guideHeight);
    SetTuning();

    int r = nr.evaluate(cmd, nr.feature, p);
    nr.reset = qfalse;
    if (r != NVSDK_NGX_Result_Success) {
        /* Put the frame back to linear untouched: resolve at strength 0 would read garbage
           from the scratch image, so decode by resolving against itself is not an option -
           a failed evaluate leaves the encoded frame, and the next frame is a fresh one. */
        Fail("EvaluateFeature failed", r);
        return;
    }
    nr.frames++;

    ImageBarrier(cmd, qvk.images[VKPT_IMG_MOTION_BLUR], VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    /* The model's answer against the encoded original, back to linear in DLSS_OUTPUT. */
    Convert(cmd, 1, width, height);
}
