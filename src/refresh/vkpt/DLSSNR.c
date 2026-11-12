/*
DLSS 5 NEURAL RENDERING - engine side. See DLSSNR.h.

HOW IT IS CALLED. Through the driver's NGX core, like DLSS/RR/FG and like RTX Remix: the
instance DLSS.c's TryInit set up on this device, our own parameter block from
NVSDK_NGX_VULKAN_GetCapabilityParameters, and NVSDK_NGX_VULKAN_CreateFeature1 with feature
id 18. The core finds nvngx_dlssnr.dll next to q2rtx.exe itself, checks its signature and
calls it. Until 2026-11 this went through a forwarder DLL instead (nvngx.dll_dlssnr.dll): the
runtime only accepts calls from a module whose path contains "nvngx.dll" - normally the
core, _nvngx.dll - and the core refused the unsigned 310.8 runtime, so the forwarder called
it directly under a name that passed. 310.9.16 is signed and the core loads it, so the
forwarder is gone; 310.8 now fails at DLSSNR.Available with a message saying so.
Side benefit: the runtime's own log lines ("Created feature ... network WxH, preset=N ->
name") reach the console through TryInit's logging callback.

THE D3D12 RENDERER (vid_rtx 3) runs this same code on its own NGX instance: the API calls
are a dlss5_backend_t it hands over (d3d12/frontend.c), and it records the passes and the
evaluation itself - DLSS5Prepare, DLSS5ConvertPush, DLSS5DescribeEvaluation and
DLSS5Evaluated are DLSS5Apply in pieces, which DLSS5Apply below is made of too. Its
resources go over as D3D12 ones (SetD3d12Resource, d3d12/ngx.c). pt_dlss5_drugs is
Vulkan's only.

WHAT THE MODEL IS FED

  Color   VKPT_IMG_DLSS_OUTPUT at output resolution, tone mapped and HUD-less, ENCODED TO
          sRGB first by dlss5_convert.comp mode 0. The frame is linear until the swapchain
          blit encodes it, and the model expects what an SDR game buffer holds - sRGB. Feeding
          it linear made its colour adaptation hunt between red and washed-out blue.
          Under vid_hdr the frame is scRGB; the encode first maps it to the frame SDR mode
          would have produced and the resolve maps the model's edit back (see
          dlss5_convert.comp - clamping scRGB straight to [0,1] wrecked the colours).
  Depth   VKPT_IMG_DLSS_DEPTH, render resolution, not inverted - same image DLSS gets.
  MVec    VKPT_IMG_PT_DLSS_MOTION, render resolution, UV units, so the scale is the render
          size - again exactly what DLSS gets (InMVScaleX = render width).
  Output  VKPT_IMG_MOTION_BLUR. That is scratch for the motion blur pass, which runs before
          tone mapping and copies its result out, so it is free here; it is rgba16f and at
          least output-sized. Using a global image lets the resolve shader read the model's
          answer and the original side by side (mode 1), which is what makes the strength,
          colour and difference controls possible.
  Jitter  the sub-pixel jitter the depth and motion guides were rendered with, signed the
          way DLSS gets it (pt_dlss_jitter_sign) - RTX Remix sends its camera jitter too.
  Mask    VKPT_IMG_DLSS5_MASK as DLSSNR.ControlMask (RGBA8: R intensity, G local tone,
          B local structure, per pixel), render resolution, while pt_dlss5_volumetric is on
          - RTX Remix's "Volumetric Improvement", which multiplies its per-material mask by
          the fog/volumetric/alpha-blend transmittance so the model leaves fog alone. We
          have no per-material values, so ours is (t, t, t, 1): t the scene's share of the
          pixel's light, the rest being fog in-scatter and the transparent overlay
          (dlss5_scene_share in shader/brdf.glsl - why a share and not a transmittance is
          there). Sky and emissive count as scene. Off, no mask: NGX treats it as all 1s.

THE PARAMETERS are the ones in NVIDIA's DLSS 5 header (nvsdk_ngx_defs_dlssnr.h, 310.9.16,
shipped in the SDK package RTX Remix builds against - not on the public DLSS GitHub), set
with the types its Vulkan helper uses: SetI for flags, enums and the preset, SetUI for sizes
and the style, SetF for strengths and scales. Mask blend and skin strength follow RTX Remix:
with the auto mask on, the blend is Min and the skin strength the cvar; off, None and 0.

TUNING IS LIVE. The header lists style, intensity, the strengths, the auto mask, the mask
blend and UI correction as evaluate parameters, and RTX Remix sets them on every evaluate and
rebuilds its feature only when the output size changes. Only the preset (which network) is a
creation parameter, so only pt_dlss5_preset rebuilds the feature, once the value has held
still for DLSS5_REBUILD_DELAY_MS. (Until 2026-11-12 every tuning cvar rebuilt it - a rule
from OptiScaler's 310.8 days.) pt_dlss5_strength, _colour and _compare are ours, applied in
the resolve, and are live as well.

FEEDBACK (pt_dlss5_drugs 1). Pauses the game and feeds the model its own finished output:
after the resolve, DLSS_OUTPUT is copied into a private image, and on every following frame
that image is copied over the freshly rendered DLSS_OUTPUT before the encode, so the path
traced frame is ignored and the model keeps re-processing its last answer. The first frame
seeds the loop with the real frame. The model's history is not reset while it runs. What
gets fed back is the resolved frame, so pt_dlss5_colour 0 still locks the hue and only the
luminance drifts; pt_dlss5_colour 1 lets the model's colour compound too, and
pt_dlss5_strength above 1 speeds it up.
*/

#include "vkpt.h"
#include "DLSS.h"
#include "DLSSNR.h"
#include "system/system.h"

/* nvngx_dlssnr.dll exists for Windows only, so elsewhere DLSS 5 is never available:
   the cvars are registered as usual and pt_dlss5_available stays 0. */
#ifdef _WIN32
#include <windows.h>
#endif

#define DLSS5_REBUILD_DELAY_MS 300

/* DLSS 5 Neural Rendering. Not in the NGX SDK headers this project builds against (310.6
   stops at NVSDK_NGX_Feature_Reserved17); NVIDIA's DLSS 5 header defines
   NVSDK_NGX_Feature_DLSSNR as Reserved18. The static lib passes the id through to the core
   unchecked, and the core routes 18 to nvngx_dlssnr.dll. */
#define DLSS5_NGX_FEATURE ((NVSDK_NGX_Feature)18)

/* NVSDK_NGX_DLSSNR_Mask_Blend_Mode: how the control mask and the skin auto mask combine.
   0 (zero-initialised evaluate params) keeps the control mask's precedence. */
#define DLSS5_MASK_BLEND_NONE 0
#define DLSS5_MASK_BLEND_MIN  2

/* PFN_NVSDK_NGX_DLSSNR_GetStatsCallback, registered under "DLSSNRGetStatsCallback": writes
   the snippet's aggregate VRAM into "SizeInBytes" of the block it is given. */
typedef NVSDK_NGX_Result (NVSDK_CONV *dlss5_stats_callback_t)(NVSDK_NGX_Parameter* params);

static struct {
    qboolean failed;            /* sticky until pt_dlss5 is toggled */
    char reason[256];
    int lastResult;

    NVSDK_NGX_Parameter* params; /* ours, from GetCapabilityParameters; dies with NGX */
    NVSDK_NGX_Handle* feature;
    uint32_t width, height;     /* what the feature was built for */

    qboolean reset;
    qboolean dirty;
    unsigned dirtyTime;
    unsigned long long frames;

    /* pt_dlss5_drugs */
    VkImage feedback;
    VkDeviceMemory feedbackMemory;
    uint32_t feedbackWidth, feedbackHeight;
    qboolean feedbackSeeded;    /* the image holds a result to feed back */
    qboolean drugsPaused;       /* we set cl_paused, so we clear it */
} nr;

static VkPipeline       convert_pipeline;
static VkPipelineLayout convert_pipeline_layout;

static cvar_t* cvar_pt_dlss5;
static cvar_t* cvar_pt_dlss5_preset;
static cvar_t* cvar_pt_dlss5_style;
static cvar_t* cvar_pt_dlss5_intensity;
static cvar_t* cvar_pt_dlss5_local_structure;
static cvar_t* cvar_pt_dlss5_local_tone;
static cvar_t* cvar_pt_dlss5_skin;
static cvar_t* cvar_pt_dlss5_automask;
static cvar_t* cvar_pt_dlss5_ui_correction;
static cvar_t* cvar_pt_dlss5_volumetric;
static cvar_t* cvar_pt_dlss5_compare;
static cvar_t* cvar_pt_dlss5_strength;
static cvar_t* cvar_pt_dlss5_colour;
static cvar_t* cvar_pt_dlss5_drugs;

/* THE API HALF (DLSSNR.h's dlss5_backend_t): Vulkan's NGX calls, unless the D3D12 renderer
   handed over its own (DLSS5_SetBackend). */
static NVSDK_NGX_Parameter* VulkanInstanceParams(void)
{
    return DLSSCapabilityParameters(qvk.device);
}

static NVSDK_NGX_Result VulkanNewParams(NVSDK_NGX_Parameter** params)
{
    return NVSDK_NGX_VULKAN_GetCapabilityParameters(params);
}

static void VulkanDestroyParams(NVSDK_NGX_Parameter* params)
{
    NVSDK_NGX_VULKAN_DestroyParameters(params);
}

static NVSDK_NGX_Result VulkanCreate(void* cmd, NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** feature)
{
    return NVSDK_NGX_VULKAN_CreateFeature1(qvk.device, (VkCommandBuffer)cmd, DLSS5_NGX_FEATURE, params, feature);
}

static NVSDK_NGX_Result VulkanRelease(NVSDK_NGX_Handle* feature)
{
    return NVSDK_NGX_VULKAN_ReleaseFeature(feature);
}

static void VulkanWaitIdle(void)
{
    vkpt_device_wait_idle();
}

static const dlss5_backend_t vulkanBackend = {
    .instance_params = VulkanInstanceParams,
    .new_params = VulkanNewParams,
    .destroy_params = VulkanDestroyParams,
    .create = VulkanCreate,
    .release = VulkanRelease,
    .wait_idle = VulkanWaitIdle,
    .feedback = qtrue,
};

static const dlss5_backend_t* api = &vulkanBackend;

void DLSS5_SetBackend(const dlss5_backend_t* backend)
{
    api = backend ? backend : &vulkanBackend;
}

VkResult vkpt_dlss5_initialize(void)
{
    VkDescriptorSetLayout desc_set_layouts[] = {
        qvk.desc_set_layout_ubo, qvk.desc_set_layout_textures,
    };
    VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(dlss5_convert_push_t)
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

void DLSS5ConvertPush(int mode, uint32_t width, uint32_t height, qboolean hdr, dlss5_convert_push_t* push)
{
    const int compare = cvar_pt_dlss5_compare->integer;
    push->extent[0] = (int32_t)width;
    push->extent[1] = (int32_t)height;
    push->mode = mode;
    push->split = compare == 1 ? (int32_t)(width / 2) : 0;
    push->strength = cvar_pt_dlss5_strength->value;
    push->colour = cvar_pt_dlss5_colour->value;
    push->view = compare;
    push->hdr = hdr ? 1 : 0;
}

/* Writes VKPT_IMG_DLSS_OUTPUT, then a barrier so whatever reads it next - NGX, frame
   generation, the final blit - sees the result. */
static void Convert(VkCommandBuffer cmd, int mode, uint32_t width, uint32_t height)
{
    VkDescriptorSet desc_sets[] = {
        qvk.desc_set_ubo,
        qvk_get_current_desc_set_textures(),
    };
    dlss5_convert_push_t push;
    DLSS5ConvertPush(mode, width, height, qvk.surf_is_hdr ? qtrue : qfalse, &push);

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
    if (nr.feature) {
        NVSDK_NGX_Result r = api->release(nr.feature);
        if (NVSDK_NGX_FAILED(r))
            Com_WPrintf("DLSS 5: ReleaseFeature failed (0x%08X)\n", (unsigned)r);
    }
    nr.feature = NULL;
    nr.width = nr.height = 0;
    nr.reset = qtrue;
}

/* Device must be idle. */
static void DestroyFeedback(void)
{
    if (nr.feedback)
        vkDestroyImage(qvk.device, nr.feedback, NULL);
    if (nr.feedbackMemory)
        vkFreeMemory(qvk.device, nr.feedbackMemory, NULL);
    nr.feedback = VK_NULL_HANDLE;
    nr.feedbackMemory = VK_NULL_HANDLE;
    nr.feedbackWidth = nr.feedbackHeight = 0;
    nr.feedbackSeeded = qfalse;
}

void DLSS5_DestroyFeature(void)
{
    ReleaseFeature();
    DestroyFeedback();
}

void DLSS5_ReleaseNGX(void)
{
    ReleaseFeature();
    if (nr.params)
        api->destroy_params(nr.params);
    nr.params = NULL;
}

void DLSS5_Shutdown(void)
{
    DLSS5_ReleaseNGX();
    DestroyFeedback();

    Cmd_RemoveCommand("dlss5_info");
}

/* pt_dlss5_preset only: the network is chosen when the feature is created. */
static void preset_changed(cvar_t* self)
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
        api->wait_idle();
        ReleaseFeature();
    }
}

static void drugs_changed(cvar_t* self)
{
    if (self->integer) {
        if (!api->feedback) {
            Com_WPrintf("pt_dlss5_drugs: not on the Direct3D 12 renderer.\n");
            return;
        }
        if (!DLSS5Enabled())
            Com_WPrintf("pt_dlss5_drugs: DLSS 5 is not running (needs pt_dlss5 1 and DLSS on), "
                "so there is nothing to feed back.\n");
        /* Pause the way the pause key does; the server follows cl_paused in single player. */
        if (cl_paused->integer == 0) {
            Cvar_Set("cl_paused", "2");
            nr.drugsPaused = qtrue;
        }
        nr.feedbackSeeded = qfalse;
        return;
    }

    if (nr.drugsPaused && cl_paused->integer == 2)
        Cvar_Set("cl_paused", "0");
    nr.drugsPaused = qfalse;

    if (nr.feedback) {
        vkpt_device_wait_idle();
        DestroyFeedback();
    }
    /* Back to the real frame; the history is full of the trip. */
    nr.reset = qtrue;
}

/* What the NGX core says about DLSS 5 in a capability block - the same keys RTX Remix
   checks before it creates the feature. "?" means the core did not report the key. */
static void PrintCapabilities(NVSDK_NGX_Parameter* p)
{
    if (!p) {
        Com_Printf("  NGX core: not initialised on this device (DLSS unavailable?)\n");
        return;
    }
    int available = 0, needsDriver = 0, initResult = 0;
    unsigned int major = 0, minor = 0;
    const qboolean hasAvailable = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(p, "DLSSNR.Available", &available));
    const qboolean hasNeeds = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(p, "DLSSNR.NeedsUpdatedDriver", &needsDriver));
    const qboolean hasInit = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(p, "DLSSNR.FeatureInitResult", &initResult));
    const qboolean hasMin = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(p, "DLSSNR.MinDriverVersionMajor", &major))
        && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(p, "DLSSNR.MinDriverVersionMinor", &minor));

    char a[16], n[16], m[32], r[16];
    Q_snprintf(a, sizeof(a), hasAvailable ? "%d" : "?", available);
    Q_snprintf(n, sizeof(n), hasNeeds ? "%d" : "?", needsDriver);
    Q_snprintf(m, sizeof(m), hasMin ? "%u.%u" : "?", major, minor);
    Q_snprintf(r, sizeof(r), hasInit ? "0x%08X" : "?", (unsigned)initResult);
    Com_Printf("  NGX core: DLSSNR.Available %s, NeedsUpdatedDriver %s (minimum driver %s), FeatureInitResult %s\n",
        a, n, m, r);
}

/* The runtime's VRAM, through its stats callback (NVIDIA's NGX_DLSSNR_GET_STATS; RTX Remix's
   "show VRAM usage"). Aggregate for the snippet, so it is only meaningful with the feature up. */
static void PrintVram(NVSDK_NGX_Parameter* p)
{
    void* callback = NULL;
    if (!p || NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetVoidPointer(p, "DLSSNRGetStatsCallback", &callback))
        || !callback) {
        Com_Printf("  VRAM: not reported (no DLSSNRGetStatsCallback in this runtime)\n");
        return;
    }
    const NVSDK_NGX_Result r = ((dlss5_stats_callback_t)callback)(p);
    unsigned long long bytes = 0;
    if (NVSDK_NGX_FAILED(r) || NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetULL(p, NVSDK_NGX_Parameter_SizeInBytes, &bytes))) {
        Com_Printf("  VRAM: stats callback failed (0x%08X)\n", (unsigned)r);
        return;
    }
    Com_Printf("  VRAM: %.1f MiB allocated by nvngx_dlssnr%s\n", (double)bytes / (1024.0 * 1024.0),
        nr.feature ? "" : " (feature down)");
}

static void dlss5_info_f(void)
{
    Com_Printf("DLSS 5 Neural Rendering (NGX feature 18, through the driver's NGX core)\n");
    Com_Printf("  pt_dlss5 %d, DLSS %s, nvngx_dlssnr.dll next to q2rtx.exe: %s\n", cvar_pt_dlss5->integer,
        DLSSEnabled() ? "on" : "OFF (required)", Cvar_VariableInteger("pt_dlss5_available") ? "yes" : "NO");
    Com_Printf("  parameters %s, feature %s %ux%u, frames %llu\n",
        nr.params ? "allocated" : "none",
        nr.feature ? "up" : "down", nr.width, nr.height, nr.frames);
    PrintCapabilities(nr.params ? nr.params : api->instance_params());
    if (nr.params)
        PrintVram(nr.params);
    const int preset = cvar_pt_dlss5_preset->integer;
    const qboolean automask = cvar_pt_dlss5_automask->integer != 0;
    Com_Printf("  preset %d (%s), model %c, intensity %.2f, structure %.2f, tone %.2f\n",
        preset,
        preset == 2 ? "lightweight FP8 network, needs 310.9.16+" :
        preset == 1 ? "full network" : "DLL default = full network",
        'A' + (cvar_pt_dlss5_style->integer < 0 ? 0 : cvar_pt_dlss5_style->integer),
        cvar_pt_dlss5_intensity->value, cvar_pt_dlss5_local_structure->value, cvar_pt_dlss5_local_tone->value);
    Com_Printf("  auto mask %s (mask blend %s), character %.2f%s, UI correction %d\n",
        automask ? "on" : "off", automask ? "min" : "none",
        automask ? cvar_pt_dlss5_skin->value : 0.0f, automask ? "" : " (auto mask off)",
        cvar_pt_dlss5_ui_correction->integer);
    Com_Printf("  control mask: %s\n", DLSS5ControlMask()
        ? "volumetric improvement (DLSS5_MASK: the scene's share of the light, fog and transparencies excluded)"
        : "none (pt_dlss5_volumetric 0: the model treats every pixel alike)");
    Com_Printf("  strength %.2f, colour %.2f, compare %d\n",
        cvar_pt_dlss5_strength->value, cvar_pt_dlss5_colour->value, cvar_pt_dlss5_compare->integer);
    Com_Printf("  drugs %d, feedback %s\n", cvar_pt_dlss5_drugs->integer,
        !api->feedback ? "not on Direct3D 12" :
        nr.feedback ? (nr.feedbackSeeded ? "looping" : "allocated") : "none");
    if (nr.failed)
        Com_Printf("  FAILED: %s (result 0x%08X)\n", nr.reason, (unsigned)nr.lastResult);
    Com_Printf("  The runtime's own log (network size, preset -> network name) is in the console "
        "and DLSSTemp/nvngx_dlssnr*.log.\n");
}

#ifdef _WIN32
/* Module directory of q2rtx.exe, with a trailing backslash. */
static void ExeDir(wchar_t* out, size_t size)
{
    GetModuleFileNameW(NULL, out, (DWORD)size);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash)
        slash[1] = 0;
}
#endif

/* Whether nvngx_dlssnr.dll sits next to q2rtx.exe. The model is not something this
   project ships, so without it DLSS 5 stays off and the menu hides its settings
   (pt_dlss5_available, read by q2rtx.menu). Checked once at startup. */
static qboolean dlssnrPresent;

void DLSS5_InitCvars(void)
{
#ifdef _WIN32
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    ExeDir(dir, MAX_PATH);
    swprintf(path, MAX_PATH, L"%lsnvngx_dlssnr.dll", dir);
    dlssnrPresent = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
#endif
    Cvar_FullSet("pt_dlss5_available", dlssnrPresent ? "1" : "0", CVAR_ROM, FROM_CODE);

    cvar_pt_dlss5 = Cvar_Get("pt_dlss5", "0", CVAR_ARCHIVE);
    cvar_pt_dlss5->changed = enable_changed;

    /* NR's preset picks the NETWORK (its size and speed); the look is pt_dlss5_style. From
       the weight registry inside nvngx_dlssnr.dll 310.9.16:
         1 (A)  honest_tench, the full network - the only one 310.8 has
         2 (B)  hnet-crazy-cuckoo-small-fp8, a lightweight FP8 network, new in 310.9.16.
                RTX Remix always creates the feature with Preset B ("Preset B selects the
                lightweight network independently of the appearance model").
         0      the DLL's shipping default (the full network)
       Any other value is "not available in this DLL build" and falls back to the default,
       so 3 runs the full network too. Default 2: the speed-up 310.9.16 was released for.
       On 310.8, 2 is unavailable and falls back to the full network. */
    cvar_pt_dlss5_preset = Cvar_Get("pt_dlss5_preset", "2", CVAR_ARCHIVE);

    /* The rest are read live, every evaluate. Defaults and meanings are RTX Remix's
       (rtx.dlssNeuralRendering.*), whose sliders run 0..1. */

    /* The appearance model, sent as DLSSNR.Style: Remix's "Model A/B/C" (0/1/2). Remix only
       says the look is content-dependent - compare them in the scene. */
    cvar_pt_dlss5_style = Cvar_Get("pt_dlss5_style", "0", CVAR_ARCHIVE);
    /* The whole effect (Remix keeps it at 1 and has no slider for it) */
    cvar_pt_dlss5_intensity = Cvar_Get("pt_dlss5_intensity", "1.0", CVAR_ARCHIVE);
    /* "Structure Intensity": details, shadows, materials */
    cvar_pt_dlss5_local_structure = Cvar_Get("pt_dlss5_local_structure", "0.7", CVAR_ARCHIVE);
    /* "Tone Intensity": lightness, darkness, colour */
    cvar_pt_dlss5_local_tone = Cvar_Get("pt_dlss5_local_tone", "0.3", CVAR_ARCHIVE);
    /* "Character Intensity": structure changes on characters the auto mask finds. Sent as 0
       while the auto mask is off, as Remix does. */
    cvar_pt_dlss5_skin = Cvar_Get("pt_dlss5_skin", "0.5", CVAR_ARCHIVE);
    /* "Enable Auto Mask": the model's own character mask, combined (Min) with the control
       mask below. */
    cvar_pt_dlss5_automask = Cvar_Get("pt_dlss5_automask", "1", CVAR_ARCHIVE);
    /* "Volumetric Improvement" (Remix's rtx.dlssNeuralRendering.enableVolumetricControlMask,
       on by default there too): send DLSS5_MASK as DLSSNR.ControlMask, so the model leaves
       fog, particles, sprites, beams, explosions and Flow gas alone and works on the scene
       behind them. 0 sends no mask - NGX then treats every pixel as 1. Live. */
    cvar_pt_dlss5_volumetric = Cvar_Get("pt_dlss5_volumetric", "1", CVAR_ARCHIVE);
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
    /* Pause and loop the model's output back into itself. Not archived. */
    cvar_pt_dlss5_drugs = Cvar_Get("pt_dlss5_drugs", "0", 0);
    cvar_pt_dlss5_drugs->changed = drugs_changed;

    cvar_pt_dlss5_preset->changed = preset_changed;

    Cmd_AddCommand("dlss5_info", dlss5_info_f);
}

qboolean DLSS5Enabled(void)
{
    return cvar_pt_dlss5 && cvar_pt_dlss5->integer != 0 && !nr.failed && qvk.supports_ngx
        && dlssnrPresent;
}

qboolean DLSS5ControlMask(void)
{
    return cvar_pt_dlss5_volumetric && cvar_pt_dlss5_volumetric->integer != 0;
}

qboolean DLSS5DrugsActive(void)
{
    return cvar_pt_dlss5_drugs && cvar_pt_dlss5_drugs->integer != 0 && api->feedback && DLSS5Enabled();
}

static qboolean EnsureFeedback(uint32_t width, uint32_t height)
{
    if (nr.feedback && nr.feedbackWidth == width && nr.feedbackHeight == height)
        return qtrue;
    if (nr.feedback) {
        vkpt_device_wait_idle();
        DestroyFeedback();
    }

    VkImageCreateInfo img_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R16G16B16A16_SFLOAT,   /* DLSS_OUTPUT's format */
        .extent        = { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(qvk.device, &img_info, NULL, &nr.feedback) != VK_SUCCESS) {
        nr.feedback = VK_NULL_HANDLE;
        return qfalse;
    }
    ATTACH_LABEL_VARIABLE(nr.feedback, IMAGE);

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(qvk.device, nr.feedback, &mem_req);
    if (allocate_gpu_memory(mem_req, &nr.feedbackMemory) != VK_SUCCESS
        || vkBindImageMemory(qvk.device, nr.feedback, nr.feedbackMemory, 0) != VK_SUCCESS) {
        DestroyFeedback();
        return qfalse;
    }

    nr.feedbackWidth = width;
    nr.feedbackHeight = height;
    nr.feedbackSeeded = qfalse;
    return qtrue;
}

static void CopyWhole(VkCommandBuffer cmd, VkImage src, VkImage dst, uint32_t width, uint32_t height)
{
    VkImageCopy region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { width, height, 1 },
    };
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

/* Last frame's result over this frame's DLSS_OUTPUT, before the encode. */
static void FeedbackIn(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    /* Previous submission's copy-out; barriers reach back across submissions on one queue. */
    ImageBarrier(cmd, nr.feedback, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    ImageBarrier(cmd, qvk.images[VKPT_IMG_DLSS_OUTPUT],
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    CopyWhole(cmd, nr.feedback, qvk.images[VKPT_IMG_DLSS_OUTPUT], width, height);
    ImageBarrier(cmd, qvk.images[VKPT_IMG_DLSS_OUTPUT],
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

/* This frame's finished result into the feedback image, for the next frame. The resolve's
   barrier already made DLSS_OUTPUT visible to transfer reads. */
static void FeedbackOut(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    IMAGE_BARRIER(cmd,
        .image = nr.feedback,
        .subresourceRange = range,
        .srcAccessMask = nr.feedbackSeeded ? VK_ACCESS_TRANSFER_READ_BIT : 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = nr.feedbackSeeded ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL);
    CopyWhole(cmd, qvk.images[VKPT_IMG_DLSS_OUTPUT], nr.feedback, width, height);
    nr.feedbackSeeded = qtrue;
}

/* Our parameter block on the NGX instance DLSS.c set up for this device. The core's block
   is typed; every value is set with the type NGX_VULKAN_EVALUATE_DLSSNR_EXT (NVIDIA's
   helper in nvsdk_ngx_helpers_dlssnr_vk.h) uses for it, and the preset as an int, as RTX
   Remix sets it. */
static qboolean EnsureParams(void)
{
    if (nr.params)
        return qtrue;

    if (!api->instance_params()) {
        Fail("the NGX core is not initialised on this device (DLSS must be available)", 0);
        return qfalse;
    }

    NVSDK_NGX_Result r = api->new_params(&nr.params);
    if (NVSDK_NGX_FAILED(r) || !nr.params) {
        nr.params = NULL;
        Fail("NGX's GetCapabilityParameters failed", r);
        return qfalse;
    }
    /* As for DLSS.c's own block: give a released feature's VRAM back right away. */
    NVSDK_NGX_Parameter_SetUI(nr.params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

    /* Only an explicit "not available" stops us; an older core that does not report the
       key at all gets to try the create. */
    int available = 1;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(nr.params, "DLSSNR.Available", &available)) && !available) {
        int needsDriver = 0, initResult = 0;
        NVSDK_NGX_Parameter_GetI(nr.params, "DLSSNR.NeedsUpdatedDriver", &needsDriver);
        NVSDK_NGX_Parameter_GetI(nr.params, "DLSSNR.FeatureInitResult", &initResult);
        Fail(needsDriver
            ? "the NGX core says the driver is too old for this nvngx_dlssnr.dll (DLSSNR.NeedsUpdatedDriver)"
            : "the NGX core reports DLSS 5 unavailable (DLSSNR.Available 0). It refuses an unsigned "
              "nvngx_dlssnr.dll such as 310.8 - use 310.9.16 or newer",
            initResult);
        return qfalse;
    }
    return qtrue;
}

/* The evaluate-time controls, as RTX Remix sends them. */
static void SetTuning(void)
{
    NVSDK_NGX_Parameter* p = nr.params;
    const int style = cvar_pt_dlss5_style->integer;
    const qboolean automask = cvar_pt_dlss5_automask->integer != 0;
    NVSDK_NGX_Parameter_SetF(p, "DLSSNR.Intensity", cvar_pt_dlss5_intensity->value);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Style", (unsigned)(style < 0 ? 0 : style));
    NVSDK_NGX_Parameter_SetF(p, "DLSSNR.LocalStructureStrength", cvar_pt_dlss5_local_structure->value);
    NVSDK_NGX_Parameter_SetF(p, "DLSSNR.LocalToneStrength", cvar_pt_dlss5_local_tone->value);
    NVSDK_NGX_Parameter_SetI(p, "DLSSNR.UseAutoMask", automask ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(p, "DLSSNR.MaskBlendMode", automask ? DLSS5_MASK_BLEND_MIN : DLSS5_MASK_BLEND_NONE);
    NVSDK_NGX_Parameter_SetF(p, "DLSSNR.SkinStructureStrength", automask ? cvar_pt_dlss5_skin->value : 0.0f);
    NVSDK_NGX_Parameter_SetI(p, "DLSSNR.UICorrection", cvar_pt_dlss5_ui_correction->integer ? 1 : 0);
}

static qboolean EnsureFeature(void* cmd, uint32_t width, uint32_t height)
{
    qboolean rebuild = nr.feature && nr.dirty && Sys_Milliseconds() - nr.dirtyTime >= DLSS5_REBUILD_DELAY_MS;
    if (nr.feature && (nr.width != width || nr.height != height))
        rebuild = qtrue;
    if (rebuild) {
        /* Frames in flight still reference the feature. */
        api->wait_idle();
        ReleaseFeature();
    }
    if (nr.feature)
        return qtrue;

    /* NGX_VULKAN_CREATE_DLSSNR_EXT1's two (NVSDK_NGX_DLSSNR_Create_Params) plus the preset
       hint; the controls go along too, so the first frame needs nothing new */
    const int preset = cvar_pt_dlss5_preset->integer;
    NVSDK_NGX_Parameter* p = nr.params;
    NVSDK_NGX_Parameter_SetI(p, "DLSSNR.Enabled", 1);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Width", width);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Height", height);
    NVSDK_NGX_Parameter_SetI(p, "DLSSNR.Hint.Render.Preset", preset < 0 ? 0 : preset);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    SetTuning();

    NVSDK_NGX_Handle* handle = NULL;
    NVSDK_NGX_Result r = api->create(cmd, p, &handle);
    if (NVSDK_NGX_FAILED(r) || !handle) {
        Fail("CreateFeature1(18) through the NGX core failed - the NGX/DLSSNR lines above "
             "and DLSSTemp/nvngx*.log say why", r);
        return qfalse;
    }
    nr.feature = handle;
    nr.width = width;
    nr.height = height;
    nr.dirty = qfalse;
    nr.reset = qtrue;
    Com_Printf("DLSS 5: feature created at %ux%u (preset %d)\n", width, height, preset);
    return qtrue;
}

qboolean DLSS5Prepare(void* cmd, uint32_t width, uint32_t height)
{
    /* not over an HDR test pattern (hdr_display.c): the LDR model would repaint it, and
       the peak pattern's 10000-nit field would not survive the encode */
    if (vkpt_hdr_calibrating())
        return qfalse;
    return DLSS5Enabled() && EnsureParams() && EnsureFeature(cmd, width, height);
}

NVSDK_NGX_Parameter* DLSS5DescribeEvaluation(qboolean resetHistory, uint32_t guideWidth, uint32_t guideHeight,
                                             NVSDK_NGX_Handle** feature)
{
    /* the output size: the feature's (DLSS5Prepare made it for this frame's) */
    const uint32_t width = nr.width;
    const uint32_t height = nr.height;

    NVSDK_NGX_Parameter* p = nr.params;
    NVSDK_NGX_Parameter_SetI(p, "DLSSNR.Enabled", 1);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Width", width);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Height", height);
    NVSDK_NGX_Parameter_SetI(p, "DLSSNR.DepthInverted", 0);   /* DLSS_DEPTH is linear view depth */
    NVSDK_NGX_Parameter_SetI(p, "DLSSNR.Reset", (resetHistory || nr.reset) ? 1 : 0);

    /* The guides' jitter, in render pixels, on the NGX-wide keys RTX Remix sets it on; same
       value and sign as DLSS gets (DLSS.c's DLSSDescribeEvaluation). Both renderers keep
       this frame's in vkpt_refdef until the frame is recorded. */
    float jitterSign = (float)Cvar_Get("pt_dlss_jitter_sign", "-1", CVAR_ARCHIVE)->integer;
    if (jitterSign != 1.0f && jitterSign != -1.0f)
        jitterSign = 1.0f;
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_Jitter_Offset_X, vkpt_refdef.uniform_buffer.sub_pixel_jitter[0] * jitterSign);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, vkpt_refdef.uniform_buffer.sub_pixel_jitter[1] * jitterSign);

    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.ColorSubrectBaseX", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.ColorSubrectBaseY", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.ColorSubrectWidth", width);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.ColorSubrectHeight", height);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.OutputSubrectBaseX", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.OutputSubrectBaseY", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.OutputSubrectWidth", width);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.OutputSubrectHeight", height);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.DepthSubrectBaseX", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.DepthSubrectBaseY", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.DepthSubrectWidth", guideWidth);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.DepthSubrectHeight", guideHeight);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.MVecSubrectBaseX", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.MVecSubrectBaseY", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.MVecSubrectWidth", guideWidth);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.MVecSubrectHeight", guideHeight);
    NVSDK_NGX_Parameter_SetF(p, "DLSSNR.MVecScaleX", (float)guideWidth);
    NVSDK_NGX_Parameter_SetF(p, "DLSSNR.MVecScaleY", (float)guideHeight);
    /* The control mask is a guide too (DLSS5_MASK, written with the others); its subrect is
       the whole render size, as RTX Remix sets it. Only read when a mask is passed. */
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.ControlMaskSubrectBaseX", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.ControlMaskSubrectBaseY", 0u);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.ControlMaskSubrectWidth", guideWidth);
    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.ControlMaskSubrectHeight", guideHeight);
    SetTuning();

    *feature = nr.feature;
    return p;
}

qboolean DLSS5Evaluated(NVSDK_NGX_Result r)
{
    nr.reset = qfalse;
    if (NVSDK_NGX_FAILED(r)) {
        /* Put the frame back to linear untouched: resolve at strength 0 would read garbage
           from the scratch image, so decode by resolving against itself is not an option -
           a failed evaluate leaves the encoded frame, and the next frame is a fresh one. */
        Fail("EvaluateFeature failed", r);
        return qfalse;
    }
    nr.frames++;
    return qtrue;
}

void DLSS5Apply(VkCommandBuffer cmd, qboolean resetHistory)
{
    if (!DLSS5Enabled())
        return;

    /* HDR needs nothing here: the model is LDR-only, so dlss5_convert.comp hands it the
       SDR-equivalent frame and maps its edit back onto the scRGB original (push.hdr). */

    const uint32_t width = qvk.extent_unscaled.width;
    const uint32_t height = qvk.extent_unscaled.height;
    const uint32_t guideWidth = qvk.extent_render.width;
    const uint32_t guideHeight = qvk.extent_render.height;

    if (!DLSS5Prepare(cmd, width, height))
        return;

    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS_DEPTH]);
    BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_PT_DLSS_MOTION]);
    const qboolean controlMask = DLSS5ControlMask();
    if (controlMask)
        BARRIER_COMPUTE(cmd, qvk.images[VKPT_IMG_DLSS5_MASK]);
    /* Motion blur's scratch: last written/read by that pass (compute + transfer). */
    ImageBarrier(cmd, qvk.images[VKPT_IMG_MOTION_BLUR],
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_WRITE_BIT);

    const qboolean drugs = DLSS5DrugsActive() && EnsureFeedback(width, height);
    if (drugs && nr.feedbackSeeded)
        FeedbackIn(cmd, width, height);

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
    NVSDK_NGX_Resource_VK mask = ToNGXResource(qvk.images[VKPT_IMG_DLSS5_MASK], qvk.images_views[VKPT_IMG_DLSS5_MASK],
        guideSize, VK_FORMAT_R8G8B8A8_UNORM, false);

    NVSDK_NGX_Handle* feature = NULL;
    NVSDK_NGX_Parameter* p = DLSS5DescribeEvaluation(resetHistory, guideWidth, guideHeight, &feature);
    NVSDK_NGX_Parameter_SetVoidPointer(p, "DLSSNR.Color", &color);
    NVSDK_NGX_Parameter_SetVoidPointer(p, "DLSSNR.Depth", &depth);
    NVSDK_NGX_Parameter_SetVoidPointer(p, "DLSSNR.MVec", &motion);
    NVSDK_NGX_Parameter_SetVoidPointer(p, "DLSSNR.Output", &output);
    /* The block outlives the frame, so "off" has to clear what "on" set. */
    NVSDK_NGX_Parameter_SetVoidPointer(p, "DLSSNR.ControlMask", controlMask ? &mask : NULL);

    if (!DLSS5Evaluated(NVSDK_NGX_VULKAN_EvaluateFeature_C(cmd, feature, p, NULL)))
        return;

    ImageBarrier(cmd, qvk.images[VKPT_IMG_MOTION_BLUR], VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    /* The model's answer against the encoded original, back to linear in DLSS_OUTPUT. */
    Convert(cmd, 1, width, height);

    if (drugs)
        FeedbackOut(cmd, width, height);
}
