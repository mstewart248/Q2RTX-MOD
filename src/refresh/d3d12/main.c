/*
The D3D12 renderer's entry points (vid_rtx 3): the refresh hooks, the frame
loop, init and shutdown. See ROADMAP.md for what each phase adds.

A FRAME
    R_LatencySleep  NVIDIA Reflex's sleep (reflex_nvapi.c), before the client
                    samples input
    R_BeginFrame    resize if asked; wait for DXGI (frame latency object) and
                    for this frame slot's fence; reset its allocator and the
                    frame list; frame target COPY_SOURCE -> RENDER_TARGET, clear
    R_RenderFrame   the 3D view: vkpt's CPU half (frontend.c), then the
                    world pass (world.c) into the frame target
    R_Draw*         fill vkpt's 2D queue, on the CPU
    R_EndFrame      make textures for images registered since; draw the 2D
                    queue; copy the target to the backbuffer; execute the
                    upload batch, then the frame list; signal; present

The upload batch (textures, the sampler table) is a separate command list that
always executes before the frame's, on the same queue.
*/

#include "d3d12pt.h"
#include "common/cpuprof.h"     // Sys_Microseconds (vid_present_stats)

static enum {
    STAGE_NONE,
    STAGE_WINDOW,       // VID_Init
    STAGE_FRONTEND,     // materials, images, models
    STAGE_DEVICE,
    STAGE_SWAPCHAIN,
    STAGE_TEXTURES,
    STAGE_WORLD,        // the 3D view - may have failed on its own (world.c says so)
    STAGE_DRAW,         // everything: running
} stage;

static bool world_notice_shown;

static void R_Shutdown_D3D12(bool total);

static void d3d12_info_f(void)
{
    d3d12_print_info();
}

/* ------------------------------------------------------------------------- */
/* INIT / SHUTDOWN                                                            */
/* ------------------------------------------------------------------------- */

static ref_type_t R_Init_D3D12(bool total)
{
    // like vkpt: a vid_restart is a full shutdown and init either way
    (void)total;

    Com_Printf("------- R_Init (D3D12 path tracer, experimental) -------\n");
    registration_sequence = 1;
    world_notice_shown = false;

    if (!VID_Init(GAPI_D3D12))
        goto fail;
    stage = STAGE_WINDOW;

    d3d12_frontend_init();
    IMG_Init();
    IMG_GetPalette();
    MOD_Init();
    stage = STAGE_FRONTEND;

    if (!d3d12_device_init())
        goto fail;
    stage = STAGE_DEVICE;
    // ray traced sound: its own queue, scene and set (sound_trace.c)
    d3d12_sound_init();

    if (!d3d12_swapchain_create())
        goto fail;
    stage = STAGE_SWAPCHAIN;

    // NVIDIA Reflex, through NVAPI on this device
    d3d12_reflex_init();

    if (!d3d12_textures_init())
        goto fail;
    stage = STAGE_TEXTURES;

    // without the 3D view the renderer still runs: the console and menus work
    if (!d3d12_world_init()) {
        Com_WPrintf("D3D12: the 3D view failed to start - only the 2D layer is drawn\n");
        d3d12_world_shutdown();
    }
    stage = STAGE_WORLD;
    d3d12_frontend_world_init();

    if (!d3d12_draw_init())
        goto fail;
    stage = STAGE_DRAW;

    // (profiler_dump and vkpt's other device-free commands: d3d12_frontend_init)
    Cmd_AddCommand("d3d12_info", d3d12_info_f);

    Com_Printf("D3D12: the world and its models path traced and lit, with the physical sky, fog, "
               "glass, water, particles, beams, sprites, blood and NVIDIA Flow, DLSS / Ray Reconstruction, frame generation "
               "and HDR. d3d12_show <image> shows a G-buffer image, d3d12_info lists the GPU's features, "
               "profiler 1 the GPU times, pt_ser 1/2 the SER A/B.\n");
    Com_Printf("----------------------------------\n");
    return REF_TYPE_D3D12;

fail:
    Com_SetLastError("the D3D12 renderer failed to start - see the console");
    R_Shutdown_D3D12(true);
    return REF_TYPE_NONE;
}

static void R_Shutdown_D3D12(bool total)
{
    (void)total;

    if (stage >= STAGE_DEVICE)
        d3d12_wait_idle();
    d3d.frame_open = false;
    Cmd_RemoveCommand("d3d12_info");
    d3d12_frontend_clear_stretch_pics();

    if (stage >= STAGE_FRONTEND) {
        d3d12_frontend_shutdown();
        // every image's IMG_Unload: textures go to the deferred release list
        IMG_FreeAll();
    }

    if (stage >= STAGE_DRAW)
        d3d12_draw_shutdown();
    if (stage >= STAGE_DEVICE) {
        d3d12_timers_shutdown();
        d3d12_reflex_shutdown();
        d3d12_sound_shutdown();
    }
    if (stage >= STAGE_WORLD)
        d3d12_world_shutdown();
    if (stage >= STAGE_TEXTURES)
        d3d12_textures_shutdown();
    if (stage >= STAGE_SWAPCHAIN)
        d3d12_swapchain_destroy();
    if (stage >= STAGE_DEVICE)
        d3d12_device_shutdown();

    if (stage >= STAGE_FRONTEND) {
        IMG_Shutdown();
        d3d12_frontend_mesh_shutdown();
        MOD_Shutdown();
    }
    if (stage >= STAGE_WINDOW)
        VID_Shutdown();

    stage = STAGE_NONE;
}

/* ------------------------------------------------------------------------- */
/* REGISTRATION                                                               */
/* ------------------------------------------------------------------------- */

static void R_BeginRegistration_D3D12(const char *map)
{
    d3d12_frontend_begin_registration(map);
}

static void R_EndRegistration_D3D12(void)
{
    // the images freed below may still be read by a frame in flight; a map
    // load can afford to wait for them
    if (stage == STAGE_DRAW)
        d3d12_wait_idle();
    d3d12_frontend_end_registration();
}

/* ------------------------------------------------------------------------- */
/* FRAMES                                                                     */
/* ------------------------------------------------------------------------- */

// the frame's generated frames (d3d12_frontend_dlssg_record), for R_EndFrame
static unsigned int fg_generated;

static void R_RenderFrame_D3D12(refdef_t *fd, int waterLevel)
{
    if (stage != STAGE_DRAW || !d3d.frame_open)
        return;

    // cpu_bench owns the screen while it runs (a 2D pic), as on Vulkan
    if (d3d12_frontend_cpu_bench_draw(fd))
        return;

    d3d12_world_frame_t frame;
    if (!d3d12_frontend_prepare_frame(fd, waterLevel, &frame)) {
        if (!world_notice_shown && !(fd->rdflags & RDF_NOWORLDMODEL)) {
            world_notice_shown = true;
            Com_Printf("D3D12: %s - the 3D view stays black.\n",
                       d3d12_world_ready() ? "no world loaded" : "the 3D view did not start");
        }
        return;
    }
    const bool drawn = d3d12_world_render(d3d.cmd, &frame);
    // R_EndFrame_RTX's DLSSGApply, here where the frame's images are at hand: the
    // real frame is on the frame target already, as vkpt blits it before evaluating
    if (drawn && frame.dlss) {
        fg_generated = d3d12_frontend_dlssg_record(d3d.cmd);
        // pt_dlss_fg_show 1: the first generated frame in the real one's place on screen
        if (fg_generated && d3d12_frontend_fg_show_generated())
            d3d12_world_blit_generated(d3d.cmd, 1, d3d12_rtv(D3D12_RTV_FRAME_TARGET));
    }
    d3d12_frontend_finish_frame(drawn);
}

static void R_LightPoint_D3D12(const vec3_t origin, vec3_t light)
{
    (void)origin;
    VectorSet(light, 1, 1, 1);  // as vkpt's: lighting is the path tracer's business
}

static void R_BeginFrame_D3D12(void)
{
    if (stage != STAGE_DRAW || d3d.frame_open)
        return;

    // frame generation on or off: the swapchain moves queues between frames
    d3d12_frontend_fg_begin_frame();
    fg_generated = 0;

    if (!d3d12_swapchain_update())
        return;     // minimized, or the resize failed: no frame

    // vid_present_stats: the two places this thread blocks for a frame
    const bool present_stats = d3d12_frontend_present_stats_on();
    uint64_t t = present_stats ? Sys_Microseconds() : 0;
    d3d12_swapchain_wait();
    const uint64_t present_wait_us = present_stats ? Sys_Microseconds() - t : 0;

    d3d12_frame_t *frame = &d3d.frames[d3d.frame_index];
    if (present_stats)
        t = Sys_Microseconds();
    d3d12_wait_fence(frame->fence_value);
    if (present_stats)
        d3d12_frontend_present_stats(Sys_Microseconds() - t, present_wait_us);
    d3d12_collect_garbage(false);

    if (!D3D_CHECK(ID3D12CommandAllocator_Reset(frame->allocator)) ||
        !D3D_CHECK(ID3D12GraphicsCommandList4_Reset(d3d.cmd, frame->allocator, NULL)))
        return;
    d3d.frame_open = true;
    d3d12_timers_frame_begin();

    d3d12_textures_begin_frame();
    // R_BeginFrame_RTX's vkpt_vertex_buffer_upload_models, after the textures
    d3d12_frontend_upload_models();

    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = d3d.frame_target;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList4_ResourceBarrier(d3d.cmd, 1, &b);

    static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
    ID3D12GraphicsCommandList4_ClearRenderTargetView(d3d.cmd, d3d12_rtv(D3D12_RTV_FRAME_TARGET), black, 0, NULL);
}

static void R_EndFrame_D3D12(void)
{
    if (!d3d.frame_open) {
        d3d12_frontend_clear_stretch_pics();
        return;
    }

    // Reflex, where vkpt marks them: the frame's simulation is over, its submission starts
    d3d12_reflex_marker(D3D12_REFLEX_SIMULATION_END);
    d3d12_reflex_marker(D3D12_REFLEX_RENDERSUBMIT_START);

    // profiler 1: the GPU times over the HUD; tm_debug's line
    d3d12_timers_draw();
    d3d12_frontend_draw_debug();

    // images the HUD or menus registered since R_BeginFrame are drawn below
    d3d12_textures_begin_frame();

    d3d12_draw_submit(d3d.cmd, d3d12_rtv(D3D12_RTV_FRAME_TARGET), d3d.width, d3d.height);

    /* With frame generation the swapchain is on the present queue, and every frame -
       menus and photo mode too, as one present - goes out through fg_present.c's thread
       (swapchain.c's comment): each generated frame and the same HUD over it into a
       present source of this frame's set, the real frame copied into its own. */
    const bool fg = d3d12_fg_mode_active();
    const int set = (int)(d3d.frame_counter % D3D12_FG_SETS);
    const unsigned int generated = fg ? fg_generated : 0;
    // pt_dlss_fg_show 1 puts the first generated frame on screen alone, once per real
    // frame, as Vulkan's (no group): the frame target holds it (R_RenderFrame)
    const unsigned int presented = generated && d3d12_frontend_fg_show_generated() ? 0 : generated;
    if (fg) {
        for (unsigned int k = 1; k <= presented; k++) {
            d3d12_fg_source_begin(d3d.cmd, set, (int)k);
            d3d12_world_blit_generated(d3d.cmd, k, d3d12_fg_source_rtv(set, (int)k));
            d3d12_draw_submit(d3d.cmd, d3d12_fg_source_rtv(set, (int)k), d3d.width, d3d.height);
            d3d12_fg_source_end(d3d.cmd, set, (int)k);
        }
        // its source 0, made with the first frame on the present queue
        if (!d3d12_fg_sources(0))
            Com_EPrintf("D3D12: no present source for the frame\n");
        d3d12_fg_record_real(d3d.cmd, set);
    } else {
        d3d12_swapchain_record_copy(d3d.cmd);
    }

    d3d12_timers_frame_end(d3d.cmd);
    d3d12_frame_t *frame = &d3d.frames[d3d.frame_index];
    bool closed = D3D_CHECK(ID3D12GraphicsCommandList4_Close(d3d.cmd));

    d3d12_upload_flush();
    if (closed) {
        if (fg)
            d3d12_frontend_fg_before_submit(set);
        ID3D12CommandList *lists[1] = { (ID3D12CommandList *)d3d.cmd };
        ID3D12CommandQueue_ExecuteCommandLists(d3d.queue, 1, lists);
    }
    frame->fence_value = d3d12_signal();
    d3d12_stamp_garbage(frame->fence_value);
    d3d.frame_open = false;

    if (closed) {
        d3d12_reflex_marker(D3D12_REFLEX_RENDERSUBMIT_END);
        // pt_dlss_fg_compare / pt_dlss_fg_dump: this frame's finished images (each read
        // waits for the GPU)
        d3d12_frontend_fg_debug(generated);
        if (fg) {
            // the real frame's present markers are set on the present thread
            d3d12_frontend_fg_present(set, presented);
        } else {
            d3d12_reflex_marker(D3D12_REFLEX_PRESENT_START);
            d3d12_present();
            d3d12_reflex_marker(D3D12_REFLEX_PRESENT_END);
        }
    }

    d3d.frame_index = (d3d.frame_index + 1) % D3D12_FRAMES_IN_FLIGHT;
    d3d.frame_counter++;
    d3d12_frontend_clear_stretch_pics();
}

static void R_ModeChanged_D3D12(int width, int height, int flags, int rowbytes, void *pixels)
{
    (void)rowbytes; (void)pixels;
    Com_DPrintf("D3D12: mode changed %d %d\n", width, height);
    r_config.width = width;
    r_config.height = height;
    r_config.flags = flags;
    d3d.resize_pending = true;
}

static void R_AddDecal_D3D12(decal_t *d)
{
    (void)d;
}

// vid_hdr, once the swapchain is scRGB: the client's screenshots go to .hdr
static bool R_IsHDR_D3D12(void)
{
    return d3d.hdr;
}

static bool R_InterceptKey_D3D12(unsigned key, bool down)
{
    (void)key; (void)down;
    return false;
}

/* ------------------------------------------------------------------------- */

void R_RegisterFunctionsD3D12(void)
{
    R_Init = R_Init_D3D12;
    R_Shutdown = R_Shutdown_D3D12;
    R_BeginRegistration = R_BeginRegistration_D3D12;
    R_EndRegistration = R_EndRegistration_D3D12;
    R_SetSky = NULL;                // vkpt's R_SetSky_RTX: d3d12_frontend_register_functions
    R_RenderFrame = R_RenderFrame_D3D12;
    R_LightPoint = R_LightPoint_D3D12;
    R_BeginFrame = R_BeginFrame_D3D12;
    R_EndFrame = R_EndFrame_D3D12;
    R_ModeChanged = R_ModeChanged_D3D12;
    R_AddDecal = R_AddDecal_D3D12;
    R_InterceptKey = R_InterceptKey_D3D12;

    R_IsHDR = R_IsHDR_D3D12;
    R_LatencySleep = d3d12_reflex_sleep;
    R_SoundTrace = d3d12_sound_trace;
    // NVIDIA Flow's is vkpt's flow.c: d3d12_frontend_register_functions
    R_FlowEmit = NULL;

    IMG_Load = IMG_Load_D3D12;
    IMG_Unload = IMG_Unload_D3D12;
    IMG_ReadPixels = IMG_ReadPixels_D3D12;
    IMG_ReadPixelsHDR = d3d12_read_frame_target_hdr;

    // the 2D functions and model loaders are vkpt's, unchanged
    d3d12_frontend_register_functions();
}
