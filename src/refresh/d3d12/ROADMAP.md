# The D3D12 path tracer (`vid_rtx 3`)

The aim is a second backend for the same path tracer: the same shaders, materials,
lights and cvars as the Vulkan renderer, on Direct3D 12. That gives a fair
Vulkan-vs-D3D12 performance A/B, plus access to the ray tracing features that
ship on D3D12 first: DXR 1.2 and the Agility SDK previews.

**Status (2026-11-06): phase 1 (the 2D layer) works in game. Phase 2 draws the world lit -
vkpt's lighting passes, A-SVGF, TAA and tone mapping, with TAA_OUTPUT on screen - and Matt
saw it in game on 11-05, then the physical sky, the sun and skyboxes (`sky.c`), then
reflection and refraction (glass, water, mirrors). Dynamic models (monsters, items, the
view weapon), the effects TLAS (particles, beams, sprites, explosions), blood, SHaRC and
bloom are built and tested headless (`d3d12_world_test` phases 2 and 5 to 7), not yet seen in
game. DLSS Super Resolution and Ray Reconstruction run through NGX's D3D12 path - headless,
for real, on the system runtime (see "DLSS and the Agility SDK") - and Matt confirmed them in
game, as he did the fog - god rays, map fog on the froxel grid, the sun's shadow map -
on 11-06, then frame generation and HDR output. The retail Agility runtime (SM 6.9) now
comes from a run-time-written `D3D12SDKVersion` export, with DLSS on it too, and Matt ran
the SER A/B (`pt_ser`) on it: no gain, so it stays off.** Since 11-06 it is in the menus
(THE MENUS, below).

- Phase 2 done:
  - the binding layer (generated root signatures for all 49 compute shaders, descriptor
    set instances, compute and graphics binding);
  - the acceleration-structure layer (classic BLAS/TLAS, plus a partitioned TLAS backend
    for RT tier 2.0);
  - vkpt's map registration and per-frame CPU half, shared by both backends (below);
  - `world.c`: the world buffer and static BLASes at map load, then per frame the UBO,
    light and instance buffers, the TLAS, then `passes.c`'s frame (below), then a blit of
    TAA_OUTPUT - or of any G-buffer image (`d3d12_show`).
  - `passes.c` (engine-free, so the test records the same frame): `primary_rays`,
    `direct_lighting` (and its caustics variant), `indirect_lighting` (first and later
    bounces, half height at `pt_num_bounce_rays 0.5`), ReSTIR GI temporal + spatial,
    A-SVGF (gradients, temporal, 4 a-trous iterations with the low-frequency passes) or
    plain compositing, the checkerboard interleave, TAA, and SDR tone mapping - vkpt's
    order, dispatch sizes and push constants.
  - The lighting's inputs: blue noise (`textures.c`), the three light statistics buffers,
    the tone mapping buffer, specialization-constant DXIL variants (`--spec`, below).
  - The sky (`sky.c` records, `world.c` owns the resources): vkpt's scatter tables, clouds
    and terrain cubes from `env/*.dds` (`d3d12_dds_parse`: LoadImageFromDDS's rules), the
    1024² RGBA16F envmap cube, the 2048² terrain shadow map drawn from the heightmap grid,
    `physical_sky.comp` / `physical_sky_space.comp` and `sky_buffer_resolve.comp` into the
    sun colour buffer - when vkpt's `skyNeedsUpdate` asks, as on Vulkan. `R_SetSky` is
    vkpt's `R_SetSky_RTX`; its skybox goes up through the `upload_envmap` hook.
  - `reflect_refract` (glass, water, mirrors, screens, security cameras): `pt_reflect_refract`
    bounces after the primary rays, vkpt's two variants without Flow (`spec_1_0` first,
    `spec_0_1_1_0` later) - or with it on a frame with a Flow grid (the base file and
    `spec_0_1`, as `pt_reflect_noflow` picks) - each over its tile list (tile compaction,
    below) or the whole launch grid.
    The water normal map it reads (`textures/water_n.tga`) is found by both backends now
    (`vkpt_find_permanent_images`).
  - Dynamic models (md2/md3/iqm: monsters, items, the view weapon, the player model):
    - Each model's VBO is uploaded at R_BeginFrame, as vkpt's `vkpt_vertex_buffer_upload_models`
      does, through the shared slot walk (`vkpt_model_vbo_action` / `_layout` / `_stage`).
      A static model also gets its three BLASes, built in the upload batch.
    - The instanced buffers follow `pt_primbuf`, growing by vkpt's rule.
    - The IQM bones are copied per frame.
    - `instance_geometry.comp` and `animate_materials.comp` run before the TLAS.
    - The five model sections' BLASes are rebuilt every frame, in memory of the frame slot's
      own (`accel.c`'s plan/build). They are added to the instance list by
      `vkpt_pt_append_dynamic_instances`, the function vkpt's own TLAS uses.
    - `pt_tlas_fast_trace` and `pt_blas_fast_trace` choose the build flags, as on Vulkan.
  - The effects TLAS (particles, beams, sprites, explosions):
    - `transparency.c`'s CPU half is shared. `vkpt_transparency_prepare` counts the frame's
      particles, beams and sprites and writes them into its CPU buffer, and
      `vkpt_transparency_frame` describes the layout. Vulkan's `update_transparency` is that
      plus its upload.
    - `world.c` copies each stream from the frame slot's staging to the start of its own
      buffer, as `upload_geometry` does. The index list is uploaded once, and the four texel
      buffers are bound in each slot's RT set.
    - The per-frame plan covers all ten of vkpt's dynamic BLASes. The explosions are a run of
      the instanced positions, the particles' and sprites' quads go through the one index
      list, and the beams are AABBs (`accel.c`'s procedural geometry).
    - Each frame slot has a second TLAS (`TLAS_INDEX_EFFECTS`), built from the instance list
      past `num_geometry_instances`. It is built even when empty, so effects rays never read
      stale memory.
  - Blood (`cl_blood_spheres`):
    - `blood.c`'s CPU half is shared. `vkpt_blood_prepare` chooses the LODs, writes the
      droplets into the shadow copy of the section, runs the epoch and dirty-slot logic,
      and returns the regions that have to reach the device (`vkpt_blood_frame_t`).
      Vulkan's `vkpt_blood_update` is that plus its own staging, fill, copies and barrier.
      Its staging moved into `blood_vk`, made when the shadow's size changes.
    - `world.c`'s `upload_blood` memcpys the regions into the frame slot's staging (grown
      to the shadow's size). Then `passes.c`'s `d3d12_record_blood_upload`, on an epoch
      frame, zeroes the section by copying from the zero buffer, only in the gaps between
      the regions, so no two copies write the same bytes. Then it copies the regions. All
      of this runs before `instance_geometry`, as Vulkan records it.
    - The blood BLAS is one more run of the instanced positions. **Settled blood keeps it
      (`pt_blood_blas_reuse`, ported 11-06):** frontend.c asks blood.c
      (`vkpt_blood_blas_needs_build`, the same records Vulkan uses, per frame slot) and
      tells it after a build (`vkpt_blood_blas_built`). world.c marks the blood run
      PERSISTENT (accel.h): `accel.c` keeps it in the slot's own memory (`persist`), not
      in the per-frame pool, and lets it stand unbuilt only when that memory holds a BLAS
      of the very same geometry (vertex address, counts) and flags. So a renderer
      restart or a remade buffer can never hand back a stale one. Measured before the
      port: the blood rebuild was ~0.2 ms per 500 splats (0.8 ms at 2261) every frame.
    - A D3D12 rebuild of the instanced buffers bumps `vkpt_primbuf_new_generation`, which
      starts a new epoch. So does a dropped upload (no memory for the staging), through
      `d3d12_world_take_blood_lost` and `vkpt_blood_invalidate`.
  - The client's renderer gates: it made blood droplets only for `REF_TYPE_VKPT`, so
    `vid_rtx 3` showed the classic particles (Matt, 11-06). `CL_PathTraced()` (client.h)
    now covers both path tracers wherever vkpt's shared frontend does the work: blood,
    colour shells on the base entity, flares, the first-person player model drawn only in
    mirrors, beam glow, the analytic dynamic lights, the flashlight, the light editor's
    lights, explosion lights and rail lights. Since 11-06 also the FPS counter's
    resolution scale (`scr_fps 2`, screen.c): frontend.c feeds `feedback.resolution_scale`
    back from `vkpt_drs_update` every frame. No `REF_TYPE_VKPT`-only gate is left in the
    client's path tracer code.
  - **The console commands (11-06):** `vkpt_register_commands` / `vkpt_unregister_commands`
    (vkpt main.c) hold the ones that need no device. `R_Init_RTX` and
    `d3d12_frontend_init` both call them: `fog` (fog volumes), `camera`, `cpu_bench`,
    `fog_debug`, `profiler_dump`, `reload_textures`, `show_pvs` (the PVS overlay, through
    the UBO), `next_sun`. D3D12 had only `profiler_dump` before. `reload_shader` stays
    Vulkan's. Before, `fog_debug` was never removed at a Vulkan shutdown.
    - **`mat` (11-06 eve, built, not run):** MAT_Init registers it on both renderers, but
      `d3d12_frontend_init` used to remove it again. The reason was that
      `vkpt_vertex_buffer_invalidate_static_model_vbos` (`mat reload`, `mat <attribute>`,
      `mat create_instance`) waits on the Vulkan device and frees Vulkan buffers. Matt
      found `mat print`, `mat reload` and the edits missing.
      - Two new `vkpt_prep_backend` hooks do that part, `wait_idle` and `free_model`, both
        NULL on Vulkan. D3D12's are `d3d12_wait_idle` and `d3d12_world_model_free` +
        `vkpt_model_vbo_clear`.
      - The next frame's `d3d12_frontend_upload_models` sees the slot as UPLOAD and builds
        it again, reclassified from the new material.
      - The crosshair's material comes from primary_rays' readback, through
        `process_render_feedback` in the shared `vkpt_prepare_frame`.
    - **Photo mode's free camera (11-06 eve, built, not run):** `R_InterceptKey_D3D12` was
      a stub that returned false, so WASD/QE, the depth-of-field wheel and the mouse-button
      swallowing never reached freecam.c, and the camera sat still. Now main.c sets the hook
      NULL and `d3d12_frontend_register_functions` sets vkpt's `R_InterceptKey_RTX`. The
      per-frame half, `vkpt_freecam_update`, already ran in the shared `vkpt_prepare_frame`.
    - `reload_textures` needed `IMG_Load_D3D12` to make an existing slot's texture again.
      It used to keep the new pixels but never re-upload them, so an `IMG_ReloadFrom`
      changed nothing on screen. Now the old texture goes as an unload's does (a deferred
      release, the slot null), and the next frame's `d3d12_textures_begin_frame` makes it
      from the new pixels.
    - **cpu_bench:** R_RenderFrame hands the screen to `vkpt_cpu_trace_draw` (a 2D pic)
      while the bench runs or its result is up (`d3d12_frontend_cpu_bench_draw`, Vulkan's
      first test). The capture was always in the shared `vkpt_prepare_frame`. Its
      physical sky is this renderer's cube: a new prep-backend hook, `read_sky_envmap`,
      is `d3d12_world_read_sky_envmap`, which copies the six RGBA16F faces out through
      the upload list and waits. Without the hook it fell back to a flat sky colour.
  - **vid_present_stats (11-06):** the report is vkpt's (`report_present_stats`, now told
    the presentation's name and image count). `R_BeginFrame_D3D12` times its two waits:
    the frame slot's fence ("fence"), and the swapchain's latency wait ("acq"). With frame
    generation, the wait for the present thread in `d3d12_frontend_fg_before_submit`
    counts as "acq" too. They go to `vkpt_present_stats_frame`. GPU ms is the same
    profiler row (`PROFILER_GPU_FRAME`, written every frame). The mode reads "vsync",
    "tearing", "no vsync, no tearing" or "FG present thread".
  - **The debug views (11-06, built, not seen):**
    - `pt_dlss_debug` already worked (the frontend's DLSS arm reads it).
    - `tm_debug`: the histogram and the curve were always drawn (`tone_mapping_apply`,
      UBO). Now the adapted-luminance line too: `d3d12_frontend_draw_debug` calls
      `vkpt_tone_mapping_draw_debug` in R_EndFrame, before the 2D is submitted.
    - `bloom_debug` 1..3: the downscale, the horizontal blur or the whole blur over the
      frame in place of the composite, tone mapped after, as `bloom_debug_show_image`.
      `postfx_copy.comp` gained modes 3 (into TAA_OUTPUT, bilinear from a quarter of the
      extent) and 4 (into DLSS_OUTPUT, nearest, the DLSS bloom images), and a fourth push
      dword picking the vertical or horizontal blur's image.
    - `pt_dlss_fg_show 1`: R_RenderFrame blits the first generated frame over the frame
      target after DLSS-G's evaluation, and R_EndFrame presents that alone, a group of
      one, as Vulkan (whose `DLSSGShowInterpolated` asks its own feature, so it was
      always false here).
    - `pt_dlss_fg_compare`, `pt_dlss_fg_dump` 1/2: vkpt main.c's tools now read through
      a `vkpt_fg_read_t` (a region of a `VKPT_IMG_*` image, RGBA16F). Vulkan's is the old
      copy code, made per call. `vkpt_fg_debug_frames` runs both with
      `d3d12_world_read_image` (a CopyTextureRegion box on the upload list, the GPU
      waited for) after the frame's list is executed. The swapchain images are not
      dumped on D3D12 (`fgdumpN_real/genK/mv.png` are).
    - `VKPT_IMAGE_DUMPS` (a compile-time switch) is not ported.
  - **The player setup's model view (11-06):** `playerconfig.c` uses `CL_PathTraced()`
    for its transparent menu, the bounce card and the two dlights. Its frame has
    `RDF_NOWORLDMODEL`, and from the main menu no map is loaded at all. frontend.c now
    takes it, as `vkpt_prepare_frame` does: it needs a world only for a world frame. The
    frame carries `no_world`; `world.c` then renders without the world buffer. Nothing
    reads it then: there are no world instances, the fog is off (`god_rays_enabled`
    needs a world) and the sky is not recorded (its request stands until a world frame).
    The texture animation runs only with a world loaded. Flow is told `render_world`
    (no march, no simulation). The unload now writes a null descriptor over the world's
    primitive buffer. The menu's rectangle is in the projection (prepare_ubo's viewport
    matrix), and the frame covers the screen, as on Vulkan. The DLSS history reset is
    shared (`vkpt_dlss_take_history_reset(render_world)`), so photo mode's preview rule
    (reset once, leave the world's flag standing) holds on both. Built, not seen.
  - SHaRC (`pt_sharc`, the radiance cache):
    - `vkpt_prepare_frame` now decides it for both backends: `prep->sharc_active`
      (vkpt's `sharc_active`: on, not photo mode, exactly 2 bounces, a world) and
      `prep->sharc_clear` (first use, a new map, back on after a frame without it). The
      clear queued by a map load moved from `vulkan_load_world` to the shared caller.
    - `world.c` makes vertex_buffer.c's three buffers (keys, accumulators, resolved:
      ~68 MB) and binds them in the vertex buffer set. Without the memory it warns and
      keeps `pt_sharc` 0 in the UBO copy.
    - `passes.c` records `vkpt_pt_sharc_update` between the gradient reprojection and
      the lighting: the clear (copies from the zero buffer), `sharc_update.rgen` at one
      path per `pt_sharc_stride`² pixels of the whole render, then `sharc_resolve.comp`
      over all `SHARC_CAPACITY` entries.
  - Bloom (`bloom_enable`, and the menu's and item wheel's blur): `bloom.c`'s three passes
    between TAA and tone mapping - the quarter-resolution downscale, the blur across and
    down, the composite into TAA_OUTPUT by the UBO's `bloom_intensity`. The CPU half was
    already shared (`vkpt_bloom_update`, `vkpt_bloom_will_run`); the blur's Gaussian is
    now too (`vkpt_bloom_blur_params`, which Vulkan's `compute_push_constants` calls).
    `bloom_debug` 1..3 (11-06, built): see the debug views below.
  - DLSS Super Resolution and Ray Reconstruction (`pt_dlss`, `pt_dlss_dldn`; moved ahead
    of the fog on 11-06, Matt plays with RR):
    - DLSS.c's API-free half is shared. `DLSSDescribeFeature` (the create parameters,
      flags and presets), `DLSSDescribeEvaluation` (the guide images by `VKPT_IMG_*`, the
      jitter sign, the RR matrices), `DLSSClampResolution`, `DLSSFeatureFailed`,
      `DLSSAdoptParameters` (the capability block, from either API) and
      `DLSSModeSelected` (viewsize_changed without its vid_rtx 0/1). Vulkan's
      `ValidateDLSSFeature` / `DLSSApply` call them - pure moves, A/B owed.
    - `ngx.c/h` (engine-free) is the API half: `NVSDK_NGX_D3D12_Init_with_ProjectID`, the
      create and evaluate helpers, release, shutdown.
    - `frontend.c` starts NGX the first frame DLSS is on (an NVIDIA adapter), makes the
      feature in the frame's command list when the size, RR or quality changes (after a
      `d3d12_wait_idle`), and evaluates TAA_OUTPUT into DLSS_OUTPUT. `pt_dlss` changes
      resize the images the next frame instead of restarting the renderer.
    - `passes.c` calls it (the `upscale` callback) after TAA, sets its heaps again and
      rebinds the root signature NGX left, then runs bloom and tone mapping on
      DLSS_OUTPUT at the output size. With RR, TAA is folded into the interleave pass
      (`vkpt_taa_folded`) as on Vulkan. The final blit shows DLSS_OUTPUT.
    - The G-buffer goes to NGX as it is: simultaneous-access textures in COMMON, which
      promote to what NGX reads and writes. GPU-based validation raised nothing on the
      evaluations.
    - Shared fix: `InitDLSSCvars` ended with `viewsize_changed`, whose vid_rtx 0/1 is a
      no-op on Vulkan but turned `vid_rtx 3` into an archived 1 on every D3D12 start, so
      the next restart came up in Vulkan. It calls `DLSSModeSelected` now.
    - Photo mode's DLSS bypass (11-06, after Matt saw photo mode not working on D3D12):
      while `vkpt_fg_bypassed` (vkpt's `accumulation_bypasses_dlss`), `frame->dlss_bypass`
      skips NGX and `postfx_copy.comp`'s third mode puts the accumulated TAA_OUTPUT in
      DLSS_OUTPUT (vkpt_blit_accumulated_to_dlss_output; bilinear if the sizes differ), and
      every such frame asks for a DLSS history reset. Before it, RR denoised the
      accumulation against a jitter the photo-mode rays ignore. The render scale is pinned
      as `drs_process` pins it (main.c's `drs_photo_mode_scale`): 100% without DRS, so it
      is a copy.
    - **Render-resolution changes remake nothing (11-06, Matt: photo mode paused on the
      way in and ran at a low frame rate after, "if the render resolution can't change
      gracefully that's what is breaking photo mode").** Until then every change of render
      scale remade the whole G-buffer behind `d3d12_wait_idle` (`ensure_images` compares
      the extents, and under DLSS the TAA images were the render extent's), and remade the
      DLSS feature. Now, shared with Vulkan (vkpt main.c):
      - `render_scale_capacity`: the images are made for the largest scale reachable
        without a settings change - DRS's top scale, else viewsize and, while photo mode
        is allowed and `pt_photo_mode_reserve` 1 (default, archived), photo mode's 100%.
        The screen images' split-fields width is twice THAT render width (it was twice the
        current one, so on Vulkan every DRS step past 50% rebuilt), and the TAA images are
        its extent under DLSS (`get_taa_image_extent`; the UBO's `taa_image_width` is the
        allocated size, which the shaders' UVs divide by). Cost at 4K with split fields:
        7.9 GB of G-buffer all the time instead of 3.9 GB (Matt's photo-mode peak was
        14.2 GB of 16 with the old 9.3 GB set). `pt_photo_mode_reserve 0` = remade on the
        way in and out, as before.
      - `DLSSFeatureResolution` (DLSS.c): the feature is made for `vkpt_dlss_input_capacity`
        (DRS's top, else viewsize - not photo mode's, which bypasses DLSS) or the frame's
        own extent where larger, and evaluated over the frame's extent as
        `InRenderSubrectDimensions`. Vulkan hands NGX the images' real size (`imageSize`).
      - DRS runs on D3D12: `vkpt_drs_update` (drs_process over the renderer's own
        PROFILER_FRAME_TIME, `drs_last_frame_world` through `vkpt_finish_frame`), the HUD's
        scale readout from it, and the DRS menu rows for `vid_rtx 3`.
      - `ensure_images` (when it does run) frees the old set right after the idle wait
        instead of through the deferred list, applies vkpt's image profile (the A-SVGF
        chain 1x1 under RR or flt_enable 0, `vkpt_screen_image_active`), and prints the
        size, the time and the video memory against the DXGI budget.
      - DLSS-G too, since 11-06 (both renderers): made with `DynamicResolutionScaling` for
        `DLSSFeatureResolution`'s extent (`dlssg_ngx.hpp`), and kept while that covers the
        frame's (`frontend.c` `dlssg_evaluate`, vkpt `ValidateDLSSGFeature`). Each
        evaluation already named the frame's extent as the depth and motion vectors'
        subrect. So a DRS step with frame generation on no longer remakes it behind an idle
        wait. Built, not seen in game.
      - Not eye-checked yet: photo mode in and out, a DRS run, Vulkan photo mode.
    - **DLSS 5 Neural Rendering (`pt_dlss5`; 11-06, built, not run - Vulkan's never ran
      either).** vkpt/DLSSNR.c is shared:
      - Its NGX calls are a `dlss5_backend_t` (DLSSNR.h): Vulkan's by default, this
        renderer's from `d3d12_frontend_init` (`d3d12_dlss5_backend`, back to Vulkan's at
        its shutdown and in `R_Init_RTX`). The capability block is this instance's
        (`ngx_params`); ngx.c gained `d3d12_ngx_new_params` / `_destroy_params`,
        `d3d12_ngx_dlss5_create` (`NVSDK_NGX_D3D12_CreateFeature`, feature id 18) and
        `d3d12_ngx_dlss5_evaluate` (the four resources by `SetD3d12Resource`, then
        `EvaluateFeature_C`).
      - `DLSS5Apply` is now made of the pieces this renderer calls: `DLSS5Prepare` (the
        params, the feature made or remade), `DLSS5ConvertPush`,
        `DLSS5DescribeEvaluation` (every parameter but the resources), `DLSS5Evaluated`.
        `vkpt_dlss5_take_history_reset` is the reset flag both read.
      - passes.c after tone mapping, frame `dlss5` (DLSS on, not photo mode,
        `DLSS5Enabled`): the `neural` callback's stage 0 (`d3d12_frontend_dlss5_record`:
        the feature, or "not this frame"), `dlss5_convert.comp` mode 0, stage 1 (the
        evaluation into MOTION_BLUR), mode 1 unless it failed. `after_ngx` restores the
        heaps and the root signature after each NGX call (and after DLSS's).
        `d3d12_pso_test`: 51 of 51 passes.
      - `pt_dlss5_available` is no longer cleared, so the menu's DLSS 5 rows show when
        `nvngx_dlssnr.dll` is next to the exe. `DLSS5_Shutdown` now runs at this
        renderer's shutdown too (`dlss5_info` was added again at each restart).
      - Not here: `pt_dlss5_drugs` (its feedback loop copies a private Vulkan image; it
        says so and does not pause).
      - Unverified: that the runtime takes D3D12 resources (every DX12 game would need
        it), and that the core routes id 18 on D3D12 as on Vulkan. If it fails,
        `dlss5_info` and the console's NGX lines say why.
    - (Frame generation: below.)
- **DLSS and the Agility SDK.** NGX does not start on a device made through
  `ID3D12DeviceFactory`: `NVSDK_NGX_D3D12_Init_with_ProjectID` returns
  `FAIL_PlatformError` ("NGXGetPath failed to obtain path to models"). Probed on 11-06
  with the 1.620 runtime: the same with or without the debug layer, from any folder, and
  after `ApplyToGlobalState`. The system runtime works, and so does 1.620 taken through a
  `D3D12SDKVersion` export from the exe.
  - **The export, written at run time (11-06, `device.c`'s `export_agility_runtime`).**
    q2rtx.exe exports `D3D12SDKVersion` (0 in the file) and `D3D12SDKPath` (`.\D3D12\`).
    Just before this renderer first loads d3d12.dll, and only when
    `D3D12\D3D12Core.dll` is beside the exe, the version is set to `D3D12_SDK_VERSION`
    (620). The loader reads it then, and the device comes from plain
    `D3D12CreateDevice` on the process's 1.620 runtime: SM 6.9, DXR 1.2, and NGX starts
    on it. A probe confirmed all three cases: written 0 gave the system runtime and
    SM 6.8; written 620 gave the `D3D12\` runtime and SM 6.9; 620 without the folder gave
    0x887e0003 for every device, which is why the write is guarded.
  - After the device, `D3D12Core.dll`'s path confirms it (`caps.process_sdk`). The console
    shows "Agility SDK 620 (process)", and `d3d12_info` "(the process's,
    D3D12SDKVersion)".
  - The cost: a session that starts `vid_rtx 3` moves every d3d12.dll user in the process to
    1.620 (the same API, a newer runtime), the Vulkan renderer's included, until exit. A
    session that never starts it keeps the system runtime.
  - Fallbacks: if d3d12.dll was already loaded (an overlay, or a preview start before),
    the old rules apply. That means a factory device, or the system runtime under
    `pt_dlss`, and turning `pt_dlss` on over a factory device restarts the renderer. The
    preview runtime keeps its factory and has no DLSS.
- The system runtime's GPU-based validation (Windows 26100) reports error 1358 on the
  path tracer's first read of ASVGF_RNG_SEED in each command list: a UAV access to a
  simultaneous-access texture in COMMON, which D3D12 allows. The 1.620 layer passes the
  identical stream, legacy barriers make no difference, and the pixels are right.
  `device.c` drops 1358 on the system runtime; the world test counts it apart.
- The fog and god rays (`cl_fog`, `pt_fog_froxel`, `pt_fog_restir`, `gr_enable`; 11-06):
  - god_rays.c's and shadow_map.c's API-free halves are shared, by pure moves (Vulkan A/B
    owed):
    - the cvars (`vkpt_god_rays_init_cvars`, called from `vkpt_init_frontend` too);
    - the sky exposure bake's batches (`vkpt_sky_exposure_next_batch`, `_restart`) and its
      map fit (`vkpt_sky_exposure_fit_map`, which writes the solid mask into any buffer);
    - the shadow map's instance list (`vkpt_shadow_map_instances`), now a vertex buffer
      index and a byte offset rather than a VkBuffer;
    - `vkpt_froxel_ensure`, which with no Vulkan device makes the volumes through the
      backend's `create_froxel_volumes` hook; `vkpt_god_rays_cpu_reset`,
      `vkpt_froxel_restir_enabled`; `SKY_EXPOSURE_MASK_WORDS` moved to constants.h.
  - `world.c`:
    - the sun's shadow map (D32, SHADOWMAP_SIZE square, a layer for the opaque geometry
      and one for the water), the bake's mask, and a god rays set per frame slot, made
      at init;
    - the froxel volumes (about 150 MB) the first frame the grid is wanted, zeroed in the
      upload batch ahead of that frame. Set i writes slot i's scatter and sky volumes and
      samples the other slot's as history, as Vulkan's descriptor sets do.
  - The shadow map is drawn in two parts:
    - The world, the static models and the water go at the head of the frame's list,
      while the world and model buffers are still in COMMON. Each goes COMMON ->
      VERTEX_AND_CONSTANT_BUFFER -> COMMON around the draws, because a buffer in a read
      state cannot be promoted to the UAV that animate_materials and instance_geometry
      need next.
    - The animated models go after the dynamic BLAS build, from the instanced positions,
      which sit in NON_PIXEL_SHADER_RESOURCE | VERTEX_AND_CONSTANT_BUFFER there.
    - vkpt draws it all in one go after the BLASes.
  - `passes.c` records god_rays.c's order: the march after the primary rays, the froxel
    grid (ReSTIR's reservoir and spatial passes, a bake batch, scatter, integrate), the
    first reflection bounce, the march over it (push 1), then the filter into
    PT_TRANSPARENT. A bake batch handed to a frame that is never drawn restarts the bake.
  - Shared bug found on the way: `vkpt_prepare_frame` reads `gr_intensity` and `gr_enable`,
    which only Vulkan's `vkpt_initialize_god_rays` created, so a cold `vid_rtx 3` start
    dereferenced NULL. Until the `InitDLSSCvars` fix every D3D12 start archived
    `vid_rtx 1`, so launches came up in Vulkan first and hid it.
- Tile compaction (`pt_tile_compaction`, default 1 as on Vulkan; 11-06): the primary rays
  add each 8x8 tile holding a reflect/refract surface to list 0, and bounce b walks list
  b with `ExecuteIndirect`, adding the tiles whose next vertex is one to list b + 1. A tile
  runs the same 64 threads as on the full grid, so the picture is unchanged.
  - Shared, by pure moves (Vulkan A/B owed): `vkpt_pt_tile_layout` (the lists' sizes for
    the screen images) and `vkpt_pt_tile_header` (whether this view uses them, and their
    header). `pt_tile_compaction` is registered in `vkpt_pt_init_cvars` now.
  - `world.c`: the lists (sized by the header, remade with the GPU idle when it changes),
    a reset copy per frame slot (the header, then zeroed masks: one copy per view), a
    12-byte argument buffer and a DISPATCH command signature.
  - Two differences from vkpt, both in `passes.c`:
    - A bounce appends to the lists as a UAV while its own arguments sit in them, and no
      D3D12 state is both UNORDERED_ACCESS and INDIRECT_ARGUMENT. So each bounce's three
      argument words are copied out to the argument buffer first.
    - A dispatch holds at most 65535 groups a dimension, and 4K DLAA (`pt_dlss 5`) has
      129600 tiles, which Vulkan's limit allows. So under `VKPT_D3D12` a list's words are
      { count, row, rows, 1 }:
      - the dispatch is rows of `row` groups (256 in the game, `D3D12_TILE_ROW_GROUPS`);
      - the append adds a row with each row's first tile;
      - the last row's groups past the end return at once (`pt_tile_launch_none`).
      Vulkan's shaders are untouched: the D3D12 code is under `#ifdef VKPT_D3D12`.
- Frame generation (`pt_dlss_fg` 2..6; 11-06): NGX DLSSG called directly, as vkpt does,
  not Streamline.
  - Shared, by pure moves (Vulkan A/B owed, `pt_dlss_fg 2` and `4`):
    - `DLSSGDescribeEvaluation` holds everything DLSSGApply told the feature (the
      matrices with `pt_dlss_fg_proj`, the camera basis, the depth mode, jitter, the motion
      vector scale) in an API-free `DLSSGEvalDesc` (`vkpt/dlssg_desc.h`);
    - `vkpt/dlssg_ngx.hpp` fills NGX's create and optional evaluation parameters from it,
      for either API;
    - `fg_present.c`'s thread presents through a backend (`fg_present_backend_t`: wait for
      a frame, "is a newer one done", present), the Vulkan one by default;
    - `vkpt_get_window_gdi_device`, `DLSSGOutputImageIndex` and
      `vkpt_dlssg_take_history_reset` are public.
  - `ngx_dlssg.cpp` (engine-free; C++ because the DLSSG parameter header is) creates and
    evaluates through `NGX_D3D12_CREATE_DLSSG` / `NGX_D3D12_EVALUATE_DLSSG`.
  - `frontend.c`, after the world render:
    - remakes the feature when the output size, render size or generated count changes;
    - encodes DLSS_OUTPUT to sRGB (`dlssg_hdr.comp` mode 2, `pt_dlss_fg_srgb`), or with
      HDR to PQ (modes 0/1, `pt_dlss_fg_hdr10`) as vkpt does;
    - evaluates N times into DLSS_FG_OUTPUT..N, then decodes them all.
    - Photo mode (`vkpt_fg_bypassed`) presents each render once.
  - Presentation (the comment at the top of `swapchain.c`):
    - With frame generation on, the swapchain is remade on a high-priority present queue,
      so the path without it is untouched for the A/B.
    - `fg_present.c`'s thread does every present: on that queue, it copies a present
      source into the current backbuffer, then calls Present. Every frame goes through it
      while frame generation is on, menus included (one present each).
    - The sources come in two sets, used alternately: the real frame (a copy of the frame
      target) and up to five generated frames (the world blit of DLSS_FG_OUTPUTk, then the
      same HUD; `d3d12_draw_submit` uploads the 2D queue once a frame for this).
    - Before writing a set again, the main queue waits for the present queue's last copy
      out of it. `FGPresent_WaitUntilPending(the previous group)` first makes sure those
      copies were issued.
    - Readiness is a fence the main queue signals after each frame. The thread host-waits
      it once per group, like vkpt's timeline semaphore.
    - Pacing is always vkpt's relative schedule: the group spread over the measured
      cadence, `pt_dlss_fg_pace`, or under VRR `pt_dlss_fg_vrr_pace` plus the refresh
      floor.
    - With vsync on, Vulkan switches to an absolute schedule. Here the thread's DXGI
      latency wait (`d3d12_frame_latency`) and FIFO hold the pace instead.
    - VRR comes from NVAPI (`Reflex_DetectVRR` at Reflex's start, each resize and when the
      mode switches on, then `Reflex_PollVRR` once a second); `pt_vrr 0/1` is honoured.
  - Client: the FPS readout's "N presented (Nx FG)" is shown for `CL_PathTraced()`.
  - The debug cvars `_show`, `_compare` and the dumps: built 11-06 (the debug views,
    above).
  - **Left out on purpose (11-06), all off by default on Vulkan:**
    - `pt_dlss_fg_adapt`, the adaptive generated-frame budget. vkpt's own note: the servo
      does not converge (the budget moves the interval it is computed from; 2x/3x
      alternating, 81 changes in 89 s) - "do not switch it back on".
    - `pt_dlss_fg_split_eval`, one submit per Evaluate: the experiment for 4x returning
      every frame at the midpoint, which the create-time MultiFrameCount fix cured.
      Here it would mean closing and executing the frame's list between Evaluates.
    - The vblank phase lock (`pt_dlss_fg_vblank`): it engages only on the absolute
      schedule with `pt_dlss_fg_uncapped 0` and vsync off - the old schedule kept for
      comparison, which this renderer never had.
    - `pt_dlss_fg_indices`: it prints Vulkan's swapchain image indices.
  - The world test has a frame generation check (Tests, below). Its first run was
    stopped for low memory before it printed anything. Confirmed in game (11-06, SDR).
- HDR output (`vid_hdr`; 11-06), as vkpt's. It came up because cinematics on D3D12
  showed the dark-frame macroblocking that `ui_hdr_video_gamma` had fixed on Vulkan:
  Matt's rerelease config has `vid_hdr 1`. Vulkan applies that fix in its HDR 2D
  shader. D3D12 had only an SDR swapchain, so Windows put its frames on the HDR desktop
  with the piecewise sRGB toe, and the toe lifts the block grid. (`swapchain.c`'s
  comment.)
  - The swapchain, the frame target and the present sources are scRGB
    (R16G16B16A16_FLOAT, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`).
  - A swapchain that cannot present scRGB sets `vid_hdr` back to 0, as vkpt does.
  - `vid_hdr` is checked every frame; a change remakes the buffers in the other format,
    as a resize does.
  - HDR passes: tone mapping's apply pass runs `spec_0_1`; the 2D layer has a pipeline
    per format (`stretch_pic.frag.spec_0_1`); so does the world blit (its push
    constants' `hdr` clamp).
  - The 2D UBO is vkpt's, shared through `vkpt_draw_ui_hdr_params`: `ui_hdr_nits`,
    `tm_hdr_saturation_scale` and `ui_hdr_video_gamma`.
  - Frame generation is told about HDR through `qvk.surf_is_hdr`, set at each
    evaluation (colorBuffersHDR); a `vid_hdr` switch resets its history.
  - Hooks: `R_IsHDR` and `IMG_ReadPixelsHDR` (`.hdr` screenshots; the 8-bit reader maps
    scRGB with `ui_hdr_nits` as white, for anything else).
  - Matt confirmed it in game on 11-06: the cinematics' macroblocking is gone.
- The SER A/B (`pt_ser`): see phase 4. Measured by Matt on 11-06: the reorder loses 1 ms.
- Clusters and the partitioned TLAS: done on both renderers (11-06), see the acceleration
  structure layer.

**DirectX 12 Ultimate, or the base path.** The device needs feature level 12_0 and
DXR 1.1 ray query, nothing more. `d3d12_is_ultimate()` reports DX12 Ultimate (12_2, DXR
1.1, mesh shaders, VRS tier 2, sampler feedback, SM 6.5), and `d3d12_info` prints which
path the GPU gets. Features beyond the base path are used per feature, from its own cap,
never assumed from the Ultimate label.

```
vid_rtx 3; vid_restart     # menus, console, HUD, and the world, lit, with the sun and sky
d3d12_show PT_NORMAL_A     # any G-buffer image by its global_textures.h name (default TAA_OUTPUT: the picture)
d3d12_info                 # adapter, runtime, every DXR / SM / tier the GPU reports
vid_rtx 1; vid_restart     # back to Vulkan
```

If the D3D12 renderer fails to start, the client warns and falls back to `vid_rtx 1`.
`vid_rtx` is archived, so a broken D3D12 setup must never leave a game that dies at every launch.

## Building

1. `python setup/fetch_agility_sdk.py`. This stages the Agility SDK headers in
   `extern/agility-sdk/include` and the runtimes in `D3D12/` (retail 1.620.1) and
   `D3D12-preview/` (1.722 preview) next to `q2rtx.exe`.
2. CMake turns `CONFIG_D3D12_RENDERER` on by itself when those headers exist. It also needs
   `CONFIG_VKPT_RENDERER`, and a Vulkan SDK (`D3D12_VULKAN_SDK`, which defaults to `$VULKAN_SDK`)
   with `spirv-cross-c-shared` and `dxcompiler.dll`.
3. `ninja -C build` builds `build/tools/spv2dxil.exe`, then the `shaders_d3d12` target
   (`baseq2/shader_d3d12/*.dxil`, 56 shaders plus 11 specialization variants), then the
   client.

`q2rtx.exe` has no static `d3d12.dll`/`dxgi.dll` import. Both are loaded when
`vid_rtx 3` starts, so a build with this renderer still starts on any Windows.

## Measured on this machine (RTX 5070 Ti, Agility 1.620.1, 2026-11-05)

These numbers come from `tests/d3d12_caps_probe.c`:

| feature | reported |
|---|---|
| shader model | 6.9 |
| ray tracing tier | 1.2 (DXR 1.2: SER + opacity micromaps) |
| enhanced barriers | yes |
| GPU upload heap (ReBAR) | yes |
| work graphs | 1.0 |
| clusters + partitioned TLAS | not visible: preview runtime only |
| linear algebra (cooperative vectors) | not visible: preview runtime only |

The preview runtime (`d3d12_agility_preview 1`) returns `D3D12_ERROR_INVALID_REDIST` here
because Windows **Developer Mode is off**. Turn it on in Settings > System > For developers
before trying clusters/PTLAS. The renderer falls back to retail by itself.

## How it is put together

**One shader source.** Nothing in `baseq2/shader_d3d12` is written by hand.
`cmake/compileShadersD3D12.cmake` compiles every Vulkan GLSL shader again with
`-DVKPT_D3D12` (glslang to SPIR-V). `spv2dxil` then turns that into HLSL with SPIRV-Cross
and into signed DXIL with DXC. The handful of places where HLSL needs something else are
`#ifdef VKPT_D3D12` in the GLSL itself. A fix to a shader therefore reaches both
renderers, and the A/B compares backends, not two drifting copies.

- The renderer is **ray query only** (DXR 1.1 inline). The `.rgen` shaders build as their
  existing `.query` compute variant. SPIRV-Cross cannot emit the ray tracing pipeline
  stages (`.rchit` etc.) as HLSL.
- **Bindings:** Vulkan (set `s`, binding `b`) maps to register 0 in space `1000*(s+1)+b`.
  Push constants go to `b0 space0`, and `gl_NumWorkGroups` to `b0 space1`.
  - Storage buffers are always UAVs. An UPLOAD-heap buffer cannot be one, so a per-frame
    SBO is copied into a DEFAULT buffer.
  - Root signatures for the path tracer are generated (`bindings.c`, below). Only the 2D
    layer's (`draw.c`) is hand-written.
- **Bindless textures** are split into `texture2D[]` (binding 0), four samplers (900) and
  a byte-per-image sampler index (901). A shader-visible sampler heap holds only 2048, and
  a combined image+sampler array through HLSL would need one sampler per image.
  `vkpt_image_sampler_kind()` (vkpt/textures.c) is the one place that picks the sampler for
  both renderers.
- **The frontend is vkpt's.** That covers the 2D queue (`R_Draw*_RTX`), the model loaders,
  materials, and soon the BSP mesh and light lists. `frontend.c` is the only file here that
  includes `vkpt.h`. Nothing called from it may touch `qvk.device`, which is NULL under
  `vid_rtx 3`. The rule is to reuse vkpt's API-free code by calling it, never by copying it.
- **Runtime selection** (see "DLSS and the Agility SDK"):
  - retail: the run-time-written `D3D12SDKVersion` export, so the whole process gets
    `D3D12\`'s runtime, NGX included;
  - the preview (`d3d12_agility_preview 1`), and retail when d3d12.dll was already loaded:
    `ID3D12SDKConfiguration1::CreateDeviceFactory` on the exe-dir folder, which affects
    only that device.
- **A frame** (see main.c):
  1. Wait on the swapchain's latency object and the frame slot's fence.
  2. Record into the frame list.
  3. Draw the 2D queue into a renderer-owned "frame target" (B8G8R8A8, sRGB RTV), then copy
     it to the flip-discard backbuffer. Screenshots read the frame target.
  4. Execute the upload batch list (textures, tables) before the frame list, on the same
     queue.
  5. Present.

  Resources freed while a frame may still read them go to a fence-stamped deferred-release
  list.

## The binding layer (`bindings.h/.c`, engine-free)

- **Set layouts:** every Vulkan descriptor set layout vkpt makes is described once, from
  the same macros the shaders compile against (`LIST_IMAGES`, `*_BINDING_IDX`):
  - textures (241 bindings), ubo, vertex buffer (with `primitive_buffers[]` unbounded),
    rt (TLAS x2 + four texel buffers);
  - pt tiles (set 15), god rays, flow env, sound, normalize, sky.
- **Root signatures:** 9 pipeline layouts become 9 root signatures:
  - each set gets a resource table and a sampler table;
  - every unbounded array gets a table of its own;
  - 32 root constants carry the push constants, and 3 carry `gl_NumWorkGroups`;
  - range flags are DESCRIPTORS_VOLATILE | DATA_VOLATILE, which matches Vulkan's
    update-after-bind.

  `tests/d3d12_pso_test.c` makes every translated compute shader's PSO against them, and all
  49 pass.
- **Set instances** are bump-allocated blocks in the shader-visible heaps. The CBV/SRV/UAV
  heap is the bindless table twice (8192 each, one per frame parity), then the ring, then
  16384 for sets; the sampler heap is 4 globals, then sets, 2048 in all. Each block is
  pre-filled with null descriptors of the right kind, and typed writers mirror
  `vkUpdateDescriptorSets`.
- **The bindless table per frame parity (11-06).** Texture writes go into a CPU-only
  staging heap (textures.c); a frame's copy (`d3d12_bindless_base(frame_counter & 1)`, the
  textures sets' `external[0]` and the 2D layer's root table) gets the changed slots in
  `d3d12_textures_begin_frame`, after R_BeginFrame's wait for the last frame that read it.
  With one table, a cinematic flickered: cin.c unloads its picture and registers the next
  into the same slot every video frame, and the frame still on the GPU drew that slot null
  or the next picture before its upload had run. vkpt has a textures set per frame in
  flight for the same reason.
- **G-buffer images** are created as **simultaneous-access** textures (UAV + SRV, always in
  the COMMON layout), with enhanced-barrier *global* barriers between passes. That is
  Vulkan's GENERAL layout plus `VkMemoryBarrier`, which is how vkpt uses them. If the A/B
  shows a cost, per-image layouts are the fallback.
  - `d3d12_image_extent()` computes sizes from `LIST_IMAGES`'s own expressions.
  - The even/odd textures sets swap the `_A`/`_B` images exactly as `LIST_IMAGES_B_A` does.
- **Not yet:** the per-pass PSO table, and spec-constant variants (still SPIRV-Cross
  defaults).

## The acceleration structure layer (`accel.h/.c`, engine-free)

- **Classic:**
  - BLAS from vkpt's two geometry shapes (a plain float3 list, or uint16-indexed);
  - a TLAS from `D3D12_RAYTRACING_INSTANCE_DESC`, which is byte for byte vkpt's
    `QvkGeometryInstance_t`;
  - a bump-allocated AS pool.

  `tests/d3d12_accel_test.c` builds both shapes and a two-instance TLAS. It then traces them
  with a GLSL ray-query shader sent through glslang and spv2dxil, against the path tracer's
  root signature, and gets 256 of 256 rays right with GPU-based validation clean.
  - A BLAS takes any number of geometries. vkpt gives a static model one per mesh, and
    the hit shaders find the mesh by `GeometryIndex` (`tlas_instance_model_indices` +
    the index), so dropping one leaves a hole.
    - `accel.c` once capped a BLAS at 8. The rerelease shotgun pickups (md3s with 15 and
      10 surfaces) came out in pieces in game (11-06).
    - The test's third instance is a 48-geometry BLAS, a quad per ray. Each ray must
      report its own geometry index. With the old cap, 40 of the 48 were holes.
- **Partitioned TLAS (`pt_ptlas`; NVAPI on the retail runtime, or RT tier 2.0 on the
  preview runtime):** this is a drop-in for the per-frame TLAS, because it is traced the
  same way and instances keep their vkpt list index.
  - It persists. `d3d12_ptlas_diff` writes only the instances that changed since the last
    frame, and a null BLAS removes an index.
  - The first `num_global` instances (the static world) go to the global partition. The rest
    go to 32-instance partitions by index.
  - `tests/d3d12_ptlas_test.c` (build, in-place move + removal, no-op frame) passes on the
    NVAPI route; `preview` as its second argument takes the preview runtime (Developer
    Mode).
- **Clustered geometry (`pt_clusters`, 11-06; built, headless test passes, NOT seen in
  game).** The per-frame model BLASes (the runs of `instance_geometry`'s triangle lists:
  opaque, transparent, masked, viewer weapon, viewer models - not the explosions, not the
  blood) are built from CLASes of 85 triangles instead of whole.
  - **Blood stays whole** (11-06, Matt: "massive slow down when there is blood" on
    D3D12). blood.c pads every droplet slot to its stride with zeroed triangles, so a
    cluster holding a droplet's last faces and the padding after it has a box from the
    droplet to the world origin. A floor of splats made thousands of them, all crossing
    at the origin. A whole build sorts the zeros into their own corner; on Vulkan it also
    keeps `pt_blood_blas_reuse`. Any future run with padding or collapsed triangles must
    stay whole for the same reason.
  - **Measured 11-06 (D3D12, Matt, `pt_blood_stats 1` + `profiler 1`): a loss - leave it
    at 0.** At the same ~1000 settled splats the BVH update was 0.70 ms either way, but the
    whole GPU frame went 11.0 -> 12.5 ms with `pt_clusters 1`: tracing is slower with
    cluster support on, and the builds it could save are small. The whole dynamic BVH
    update is 0.3 ms without blood and 1.13 ms with 2261 splats (D3D12 rebuilds blood every
    frame). Blood in the air did not spike it (0.94 vs 0.91 ms). It cost about 0.5 ms of
    GPU frame and 1-3 ms of CPU (`gen` + `copy`) for the second it flew. For blood, the win
    is D3D12 blood BLAS reuse (about 0.8 ms at 2261 splats), not clusters - ported the same
    day (the blood bullet under "the world").
  - **Vulkan crash, fixed 11-06:** `destroy_accel_struct` left `present` set, and with the
    model sections clustered nothing else reset it, so the first frame after the restart
    into `pt_clusters 1` passed a NULL handle to `vkGetAccelerationStructureDeviceAddressKHR`
    (Q2RTX_CrashReport12, `vkpt_pt_create_toplevel+0x71`). Shared core
  `vkpt/clusters.c/.h` (API-free): the plan, the staging layout and NVAPI's argument
  structs, which are Vulkan's byte for byte.
  - **No ClusterID needed.** A cluster's triangles carry its index in its BLAS as their
    *geometry index*, and `PrimitiveIndex()` counts within the cluster. So
    `get_model_index_and_prim_offset` (path_tracer_hit_shaders.h) adds
    `geometryIndex * PT_CLUSTER_TRIANGLES` (constants.h) for instances without a model
    index. Every other such BLAS has one geometry (index 0), so nothing else moves, and
    spv2dxil needed no new translation.
  - The runs are unindexed (3 vertices a triangle), so every cluster reads one 8-bit index
    list 0..254, which starts the staging.
  - Per frame, two operations: **CLAS from triangles** (implicit destinations: packed into
    the slot's CLAS memory, their addresses written to a GPU array), a barrier, then **BLAS
    from CLAS** (explicit destinations, so the instance list and the PTLAS name the BLAS
    addresses on the CPU before the GPU builds anything). A cluster BLAS references its
    CLASes, so each frame slot has its own CLAS memory.
  - D3D12: `accel.c` (`d3d12_dynamic_blas_t.clustered`, planned with the classic builds in
    one pool; its scratch after theirs, so no barrier between them). NVAPI wants step 2's
    destination array in UNORDERED_ACCESS, so it is copied from the upload staging into a
    UAV buffer. `nvapi_rt.c` has the requirements, the execute call, and
    `SetCreatePipelineStateOptions(ENABLE_CLUSTER_SUPPORT)`.
  - Vulkan: `path_tracer.c` `clusters_build` through `VK_NV_cluster_acceleration_structure`
    (its own buffers per slot; ray tracing pipelines get
    `allowClusterAccelerationStructure`).
  - **A clean A/B:** a pipeline made without cluster support must not meet a clustered BLAS,
    and the support costs every traversal a little. So `pt_clusters` is CVAR_REFRESH (the
    renderer restarts). D3D12 sets the NVAPI option before `d3d12_world_init` makes its
    first pipeline, and Vulkan enables the extension and feature at device creation, both
    only while it is 1. The menu row ("clustered geometry", beside "partitioned TLAS")
    needs `pt_clusters_available`.
  - Fallbacks: if the driver gives no sizes, the runs are built whole (a warning, once).
    If NVAPI refuses the record (D3D12), `pt_clusters` is off until the renderer restarts.
  - `tests/d3d12_cluster_test.c` (+ `cluster_test.comp`): three runs (256 triangles, so
    four clusters with the last holding 1; 90; and 3 left whole) traced twice, built whole
    and clustered. 256/256 rays report the right triangle of the right run, with GPU-based
    validation clean. A clustered BLAS came out at 1 KB against 16.8 KB whole (the CLASes
    hold the triangles).
  - Not done: the D3D12 preview runtime's own cluster API (NVAPI only), and cluster
    templates (no use: the runs change shape every frame).

## Sharing vkpt's CPU half (done 2026-11-05)

Both backends now call the same functions:
- **Map registration:** `vkpt_begin_registration` (map cfgs, `BSP_Load`, the BSP mesh,
  light lists, sky settings), `vkpt_end_registration`, `vkpt_unload_world`.
- **Per frame:** `vkpt_prepare_frame` (entities, lights, UBO, TLAS instance list).

Each backend supplies a `vkpt_prep_backend_t`:
- `map_staging` / `unmap_staging`: the light, IQM matrix, light count and readback staging;
- `ensure_primbuf_size`;
- `load_sky_preset`;
- `upload_envmap`: `R_SetSky_RTX`'s skybox cube;
- `load_world`: the GPU side of a map load.

The sky's API-free halves are shared the same way: `SkySelectPreset` (the atmosphere and
the tables' file stem), `SkyBuildTerrainGrid` (the shadow map's grid from the heightmap),
`precomputed_sky_params.h` (the atmosphere struct and both planets, includable without the
engine), and `vkpt_physical_sky_reset` / `vkpt_physical_sky_set_needs_update`.

`vkpt_find_permanent_images` (the water normal map, out of `vkpt_initialize_all`) runs on
both; `prepare_ubo` names image 0 while it has not, where it used to send `NULL - r_images`
as a bindless index - which a D3D12 session started cold would have handed the shaders.

D3D12 also keeps the plain-data `qvk` fields current: extents, frame counter and index,
`device_count` 1.

The models are shared the same way (11-05). `vertex_buffer.c`'s per-slot steps are
functions that both backends' upload walks call:
- `vkpt_model_vbo_action`: keep, free or upload. `model_vbo_t::present` says the backend
  holds the buffer.
- `_layout`: the geometries and the size.
- `_stage`: the primitives and positions, into a CPU pointer.
- `_set_instance_flags` and `_mark_uploaded`.

The rest of the shared model code:
- `vkpt_primbuf_size` / `vkpt_primbuf_grow`: `pt_primbuf` and its growth rule.
- `vkpt_iqm_upload_size`: how much of the IQM staging to copy.
- `vkpt_pt_append_dynamic_instances`: the dynamic BLASes' TLAS instances, in vkpt's order
  and with its masks and flags.
- `vkpt_pt_init_cvars`, called by `vkpt_init_frontend`: the `pt_enable_*` and `pt_*_fast_trace`
  cvars, so they exist under D3D12 too.

These are pure moves for Vulkan as well, so they share the A/B below.

**`qvk.device != NULL` means "Vulkan is running"** to the shared code: the BLAS gate, the
froxel volumes and registration's idle wait. `destroy_vulkan` clears the handle and
`d3d12_frontend_init` clears it again. A stale handle after `vid_rtx 1` → `3` sent the
D3D12 frame into `vkCreateImage` on the dead device. The loader's invalid-handle check then
`abort()`ed the game with no message (fixed 2026-11-05). Any new "is this Vulkan" check in
shared code must use that predicate, and every Vulkan handle must be cleared when it is
destroyed. `vkpt_model_geometry_has_blas()` gates every instance add, so geometry
without a BLAS is skipped on both backends. On D3D12 the address alone counts: `frontend.c`
fills a static model's three from its own BLASes.

These were pure moves for Vulkan. **Vulkan needs an eye check: one screenshot A/B before and
after, any map** - one with the physical sky (`physical_sky 2`, the default) and one with a
skybox (`physical_sky 0`), for the sky moves (`FillVertexAndIndexBuffers` split, the params
header, `upload_envmap`).

The audit that planned it, kept for reference: `R_RenderFrame_RTX` (vkpt/main.c) prepares
the frame on the CPU up to its first command buffer, and the audit of everything it calls
found little Vulkan in it.

- **Pure CPU** (vkpt's own structs, plus *plain data* in `qvk` such as extents, frame
  counter and AA mode):
  - `prepare_ubo`, `evaluate_reference_mode` / `evaluate_taa_settings`,
    `process_render_feedback`, `prepare_sky_matrix`;
  - `add_dlights`, `vkpt_build_beam_lights`, `partition_bmodel_lights`,
    `bsp_mesh_animate_light_polys`;
  - `vkpt_physical_sky_update_ubo`, `vkpt_bloom_update`, `vkpt_motion_blur_update`,
    `vkpt_shadow_map_setup`, `vkpt_god_rays_prepare_ubo`;
  - `vkpt_pt_reset_instances` / `vkpt_pt_instance_model_blas`, which fill a CPU array.
- **Writes mapped Vulkan memory:**
  - `prepare_entities` writes the IQM matrix staging buffer;
  - `vkpt_light_buffer_upload_to_staging` writes the light staging buffer.

  Split each into a fill-this-pointer function and a thin vkpt wrapper that maps and calls
  it. D3D12 passes its own upload-heap pointer.
- **Touches Vulkan objects:** `vkpt_vertex_buffer_ensure_primbuf_size` (buffer resize), one
  `vkpt_device_wait_idle`, and Flow (its own hook since 11-06: `vkpt_flow_backend`, phase 3).
  These need a small backend hook.
- **BLAS addresses:** the instance list takes `model_geometry_t::blas_device_address`
  (uint64 either way). D3D12 fills that field with its own BLAS addresses.
  `QvkGeometryInstance_t` moves to a header so its size can be checked against
  `D3D12_RAYTRACING_INSTANCE_DESC`.
- **The plan:** move the block into `vkpt_prepare_frame()` returning the flags the
  recording needs (render_world, god rays / froxel on, reference mode, light counts). Both
  backends call it. D3D12 keeps the plain-data `qvk` fields current. This is a pure move
  for Vulkan, so check it with a Vulkan A/B screenshot before building on it.

## Phases

### 2: the world, path traced

- **The world: done** (`world.c`, `frontend.c`).
  - At map load: the world buffer (vkpt's `buf_world` layout, without the BLAS memory, which
    is a pool), and one static BLAS per `model_geometry_t` (5 world classes plus each brush
    model).
  - Per frame: the TLAS from vkpt's instance list as it is. A DXR instance desc is byte for
    byte vkpt's `QvkGeometryInstance_t`, flags and masks included.
- **Dynamic models: done** (`world.c`, `frontend.c`, `accel.c`). The model VBOs (vkpt's
  layout, without the BLAS memory), the instanced buffers, the IQM matrices, and the five
  model sections' per-frame BLASes.
- **The effects TLAS: done** (`world.c`, `frontend.c`, `accel.c`, `vkpt/transparency.c`'s
  shared CPU half): the explosions, particles, beams and sprites, with their texel buffers.
- **Blood: done** (`world.c`, `passes.c`, `frontend.c`, `vkpt/blood.c`'s shared CPU half):
  the droplets' section of the instanced buffers and its per-frame BLAS.
- **The binding layer: done** (above).
- **Passes**, in vkpt's frame order:
  - `instance_geometry`, `animate_materials`: **done** (`passes.c`).
  - `primary_rays`: **done**. Checkerboard fields, `pt_fullres_fields 0`.
  - the radiance cache (SHaRC: `sharc_update`, `sharc_resolve`): **done** (`passes.c`).
  - `direct_lighting` / `indirect_lighting` / ReSTIR GI: **done** (`passes.c`). The light
    counts history (`VKPT_STAGING_LIGHT_COUNTS`) stays CPU scratch: no shader reads it.
  - `reflect_refract`: **done** (`passes.c`), over the whole grid every bounce.
  - physical sky: **done** (`sky.c`). God rays, the froxel fog and the sun's shadow map:
    **done** (`world.c`, `passes.c`).
  - A-SVGF/compositing, the interleave, TAA, tone mapping (histogram, curve, apply): **done**.
    Bloom: **done** (`bloom_debug`'s views built 11-06). HDR output: **done** (11-06). Motion
    blur, chromatic aberration, film grain and the CRT: **built (11-06), not seen in game**,
    see below.
- **Post effects: built 11-06, not seen in game. `d3d12_pso_test` makes every pipeline.**
  Same cvars, same shaders, same push constants, by calling vkpt's code (pure moves):
  - `vkpt_motion_blur_params`, `vkpt_chromatic_aberration_params` (and
    `vkpt_motion_blur_bloom_takes_copy`) are the old record functions' first halves.
    Vulkan's record functions call them. The push structs moved to vkpt.h.
  - draw.c's `vkpt_final_blit_effects_params` / `_enabled` are `final_blit_params`
    without the swapchain's half. `FinalBlit_PushConstants_t` is vkpt.h's
    `vkpt_final_blit_push_t`.
  - passes.c: after the upscale (or TAA), before bloom: `motion_blur.comp`, then
    `chromatic_aberration.comp`. Each writes `MOTION_BLUR` and is copied back over the frame
    by `postfx_copy.comp`, unless bloom's downscale takes motion blur's result (vkpt's
    hand-off, the DLSS arm with CA off).
  - `postfx_copy.comp` (`d3d12/shader/`) is the D3D12 renderer's first shader of its own. It
    stands in for vkpt's `vkCmdCopyImage`, because the G-buffer images here only ever act
    as UAVs/SRVs.
  - They run only while `DLSSEnabled()` agrees with the frame's DLSS arm. The shaders pick
    their image by it.
  - Grain and CRT: frontend.c fills the blit's effect fields at the window's size.
    `record_blit` draws every picture through them: the real frame, and each FG frame
    with its own seed. `d3d12_show` views stay plain, as vkpt's `final_blit_sources`.
    `pt_crt` 1/2 first runs `crt_downsample.comp` (UBO + textures layout) into
    `MOTION_BLUR`, then draws through `crt.frag`.
- **Specialization constants: done.** SPIRV-Cross leaves a spec constant as
  `#ifndef SPIRV_CROSS_CONSTANT_ID_<id>` with its default, so a variant is the base shader's
  HLSL compiled again with `-D SPIRV_CROSS_CONSTANT_ID_<id>=<value>u`: `spv2dxil --spec
  id=value`, and `compile_shader_d3d12_variant` in cmake names the file
  `<base>.spec_<id>_<value>.dxil`. `primary_rays`' Flow pass (`1=2`) failed DXC's
  validator that way ("Flags must match usage": with every ray query specialized away, the
  file-scope `RayQuery` globals SPIRV-Cross declares are still allocated, so the shader
  declares Raytracing_Tier_1_1 and uses none). `compile_shader_d3d12_variant(... FREEZE)`
  builds it instead with `spv2dxil --freeze`: the values go into the SPIR-V with the SDK's
  `spirv-opt` (set-spec-const-default-value, freeze, fold, dead branches / functions /
  code / variables) before SPIRV-Cross sees it. The other variants keep the `#define` route.
- **Textures: mip chains done (11-06), not seen in game.** Until then D3D12 uploaded mip 0
  only, and normal maps were never renormalised. Both broke parity: the bounce samples
  base colour at mip 2, and a one-level texture reads that slower and noisier. Vulkan
  timings taken before this are not a fair A/B.
  - vkpt's rules, by calling them. `vkpt/texture_mips.h` (API-free) exports
    `vkpt_image_num_miplevels` (1 + log2 of the larger side, 1 for `IF_NO_MIPMAPS` video
    frames), `vkpt_image_has_max_mips` and `vkpt_downsample_max` from vkpt/textures.c, a
    pure un-static rename. POM height maps get vkpt's own max-filtered chain.
  - Linear (not sRGB) normal maps (`IF_NORMAL_MAP`) are renormalised on level 0 first, with
    `normalize_normal_map.comp`'s arithmetic. `pix_data` keeps what was loaded.
  - Everything else: `mipgen.c` (engine-free) reproduces vkpt's linear `vkCmdBlitImage`
    chain on the CPU:
    - a bilinear sample at each texel centre, clamped to the edge (the 2x2 average for
      even sizes, the scaled sample for odd ones);
    - sRGB colour filtered in linear light and rounded to the nearest code;
    - each level made from the stored level above.
    It runs at upload, so no UAV-flagged textures or per-level descriptors are needed. A
    2048x2048 sRGB chain costs 11.7 ms, against tens of ms for decoding that PNG.
  - **On every core since 11-06 (built, not run).** Matt: the world stayed black for about
    5 s after a level's music started. The decode runs during registration, before the
    music (precache.c's OGG_PlayTrack follows R_EndRegistration). The chains run at the
    first frame's d3d12_textures_begin_frame, after it, which is where vkpt also makes its
    textures. There they were on one thread, for hundreds of textures.
    - `make_pending_textures` now builds each texture's `build_chain` buffer (renormalised
      level 0, then every level) on all logical processors, biggest first, in batches
      of at most 512 MB.
    - The main thread then creates and uploads them as before, so the bytes don't change.
    - It prints `D3D12: N textures made in X ms (mip chains Y ms on Z threads)` for any
      burst of 64 or more.
    - `d3d12_texture_threads 1` is the old single thread, for the A/B. It isn't saved.
    - Workers use malloc, not Z_Malloc, which isn't thread-safe. `d3d12_mip_init` makes
      mipgen.c's sRGB tables before any worker starts.
  - Each level goes into the upload batch as its own copy, then every level moves to
    shader reads. The samplers already allowed every LOD (16x anisotropic).
  - Left: BC formats (vkpt has none either, so this is not a parity gap). The extended
    pixel formats were done earlier (asset choice, below).
- **First-hit hitch: fixed in both renderers (11-06), not felt in game.** The session's first
  droplet allocated blood's worst case (`cl_blood_max` x the full mesh): at 5120 and
  `pt_blood_tess 2`, 1.6M primitives = 270 MB of zeroed shadow plus the staging, on one frame.
  D3D12 was worse: one slot's staging per frame, each zeroed by a memset into upload memory
  (committed buffers already come back zeroed).
  - `vkpt_blood_preallocate` (blood.c) runs in `vkpt_begin_registration` with blood on and
    hands its capacity to `vkpt_prep_backend->reserve_blood_staging`: Vulkan's
    `vk_staging_ensure`, D3D12's `d3d12_world_reserve_blood_staging` (both frame slots, no
    memset, an exact size so a lowered cl_blood_max gives memory back at the next map).
  - The client's by-name sounds (hit marker, muzzle flashes, entity events, the game's
    first-use soundindex names) are registered with the map in `CL_RegisterTEntSounds`.
  - Diagnostic: `cl_cpustats 1` now follows each CPULONG line with `CPULONG files:`, naming
    what that frame read from disk (FS_LoadFileEx / FS_OpenFile, `cpuprof_note_file`).
- **Asset choice: done (11-06).** `models.c`'s `try_md3` and images.c's extended pixel
  formats (16-bit grey, `PF_R16_UNORM`) ask `CL_PathTraced()` now, as the client's gates do.
- **Packing:** add `shader_d3d12/*.dxil` to `shaders.pkz` (only `shader_vkpt` is packed
  today), and add `D3D12/` to the release.

### 3: DLSS and the rest of the presentation

- **NGX on D3D12:**
  - Super Resolution and Ray Reconstruction through `NVSDK_NGX_D3D12_*`: done (11-06), see
    above.
  - Frame generation: NGX DLSSG directly, as on Vulkan (not Streamline). Built 11-06, see
    above.
  - DLSS 5 neural rendering (feature 18) the same way it runs on Vulkan: built 11-06,
    see above.
- **Reflex: built (11-06), not tried in game.** `reflex_nvapi.c`, NVAPI's D3D low latency
  through `nvapi_QueryInterface` (nvapi64.dll at run time, structs by hand, size-checked).
  - The policy is vkpt/reflex.c's, split out by pure moves and called by both renderers:
    `Reflex_RegisterCvars`, `Reflex_ModeToApply` / `Reflex_ModeApplied` (fps cap, FG
    pacing, VRR cap, the console line), `Reflex_PollVRR`, `Reflex_TimingsDue` /
    `Reflex_PrintTimings` (`pt_reflex_stats`). Same cvars, same `pt_reflex_available`.
  - `R_LatencySleep` = `d3d12_reflex_sleep`: the mode, a new frame id, `NvAPI_D3D_Sleep`
    (pt_reflex 1/2, and only after a frame went to the present), INPUT_SAMPLE and
    SIMULATION_START.
  - R_EndFrame: SIMULATION_END + RENDERSUBMIT_START at the top, RENDERSUBMIT_END after
    the execute, PRESENT_START/END around `d3d12_present`.
  - Frame generation: the real frame's queue item carries its frame id and the present
    thread marks its present (frontend.c's `fg_backend_present`, and the inline
    fallback). The present queue is `OUT_OF_BAND_PRESENT` (once, when swapchain.c makes
    it), as vkpt tells its present queue.
  - A SetSleepMode failure after start turns Reflex off until the renderer restarts,
    rather than retrying (and printing) every frame.
- **HDR:** scRGB, as vkpt's (11-06, see above). An HDR10 (PQ) swapchain was not needed.
- **Ray traced sound: built (11-06), not heard.** `d3d12/sound_trace.c` is
  vkpt/sound_trace.c's GPU half on D3D12 (`s_reflections_gpu`, `s_diffraction`;
  `s_gpu_trace_available` set as vkpt sets it).
  - Shared by pure moves into `vkpt/sound_trace.h`: the run checks, scene packing (verts,
    uint32 indices, per-triangle normal + material, edges), params packing, unpacking.
  - Its own COMPUTE queue, list and fence; the simulation thread submits and waits (2 s
    timeout, then off), under a never-freed SRW lock. Nothing on that thread prints:
    buffers are made without `D3D_CHECK`.
  - A BLAS per mesh in one pool (accel.c gained `indices_32`), a TLAS with room for every
    mesh rebuilt each run, the "sound" set (bindings.c's `build_sound`). `d3d12_pso_test`
    makes both pipelines against it.
  - The fields are zeroed by a copy from a zeroed buffer (vkCmdFillBuffer's stand-in).
- **NVIDIA Flow: BUILT 11-06 (native, no interop, no copies), not seen in game.** Flow 2.2
  ships only `vulkan/` and `cpu/` device backends, so:
  - **Flow's D3D12 backend** (`setup/flow/d3d12/*`, `setup/flow/NvFlowD3D12External.h`):
    an `NvFlowDeviceInterface` on the application's `ID3D12Device` and queue, written after
    Flow's Vulkan one. Its own command lists and shader-visible heaps, executed at each
    `flush`; textures end a Flow frame in NON_PIXEL_SHADER_RESOURCE, buffers in COMMON.
    `setup/build_flow.py` copies it into the Flow source, compiles Flow's shaders to DXIL
    too (Slang -> DXC) and builds the shader headers before the libraries (Ninja decides
    what is stale before the shader tool writes them). `tests/d3d12_flow_test.c` runs 60
    frames headless under GBV and reads the atlas back: PASS.
  - **flow.c's backend hook** (`vkpt_flow_backend`, vkpt.h): Flow's device on the D3D12
    device and the frame's queue, and flow.c's Vulkan halves - the FlowParams / light sums
    / occupancy buffers, the dummies, the probe grid and the textures set's six Flow
    bindings - handed to `d3d12/flow.c`. The simulation, emitters, lights, collision and
    probe placement are flow.c's own code, unchanged. vkpt/main.c resets the hook to NULL.
  - **When:** `vkpt_flow_update` runs in `d3d12_frontend_prepare_frame` after
    `vkpt_prepare_frame` (where R_RenderFrame_RTX runs it), so Flow's work executes on the
    queue ahead of the frame's list. world.c records `d3d12_record_flow` after the TLAS:
    the occupancy's group bitset cleared (copies from the zero buffer) and
    `flow_occupancy.comp`; `flow_lights.comp`, its slice copied to a READBACK buffer
    (vkpt's is host-visible; a READBACK buffer cannot be a UAV) that `flow_lights_collect`
    reads two frames on; `flow_ambient.comp` over the slot's TLAS (the `flow env` set per
    frame slot). Then the path tracer's variants: `pt_flow_split_pass` (primary rays
    `spec_1_1`, then the march `spec_1_2`), and the reflect/refract bounces with the march.
  - `d3d12_pso_test` now also makes every pass-table entry against its own layout, as
    world.c does (50 of 50).
  - The profiler's Flow rows (11-06): `flow_profile` calls `vkpt_flow_backend->profile`
    (`d3d12_timers_flow`), a timestamp on the queue before and after Flow's flush; the
    occupancy and light sums in the frame's list are added to that row; the probes are
    FLOW_SMOKE_LIGHTING, the split march FLOW_MARCH.
  - **Not done:** `timerefresh` records the Flow passes once per view.
- **Decals: parity already.** vkpt's `R_AddDecal_RTX` is empty, so D3D12's no-op matches it.
- **THE MENUS (11-06, Matt asked).** Video > renderer offers "rtx (vulkan)" and
  "rtx (direct3d 12)" while `vid_d3d12_available` is 1 - a ROM cvar client/refresh.c sets:
  built in, and not failed to start this session (the fallback to Vulkan clears it);
  otherwise the old three-choice row. Every `ifeq vid_rtx 1` block of q2rtx.menu is
  `ifeqor vid_rtx 1 vid_rtx 3` now (the reconstruction, display, renderer, environment,
  effects, fog, blood and lens pages: shared cvars), except:
  - Vulkan only: multi-GPU (`sli`) and `ray_tracing_api`. DRS was too until 11-06, when
    it came to D3D12 with its rows (see "Render-resolution changes remake nothing").
  - DLSS 5 (11-06): its rows show as on Vulkan, by `pt_dlss5_available` (the DLL).
  - D3D12's own rows, under `ifeq vid_rtx 3`: "frame queue" (`d3d12_frame_latency`
    1-3, restarts the renderer) and, when `pt_ser_available` (world.c: shader model
    6.9), "shader execution reordering" (`pt_ser` off / on / ray generation only; not
    archived, an A/B).
- **Async compute (`pt_async_compute`): left out on purpose (11-06).** On Vulkan it moves
  the froxel fog to the compute queue, default 0, and measured no gain on the 5070 Ti
  (09-27, 3 runs an arm at 1440p pt_dlss 5: 12.77 -> 12.70 ms, 12.50 -> 12.65 ms - noise).
  The GPU is already full and the fog is ~0.6 ms. The only plausible win is hiding work
  behind DLSS, which needs per-frame device UBOs first. Here it would add a second queue,
  fences and cross-queue image hand-offs for nothing measured.

### 4: what D3D12 offers that the Vulkan path does not use (yet)

- **Opacity micromaps (DXR 1.2, retail):** alpha-tested grates, foliage and fences resolved
  in the traversal hardware instead of by candidate-hit loops in the ray query. They work
  with inline ray query.
- **Shader Execution Reordering (DXR 1.2, retail; Matt wants it as an A/B that must not
  cost anything when off). BUILT and MEASURED 11-06: no gain, default 0:** `MaybeReorderThread` exists
  only in ray generation shaders, so the indirect lighting bounces are built again as
  ones.
  - `pt_ser` (world.c, default 0):
    - 0: the compute pass;
    - 1: ray generation with `MaybeReorderThread`;
    - 2: ray generation without the reorder, which isolates the dispatch path's own cost.
    The state objects are made the first time a mode is asked for, so 0 costs nothing. It
    needs SM 6.9 (the export above) and says so once when it doesn't have it.
  - spv2dxil `--raygen` / `--ser` (`to_raygen`):
    - SPIRV-Cross's compute entry becomes `[shader("raygeneration")] void main()`, with
      `DispatchRaysIndex()` for `gl_GlobalInvocationID` (and workgroup IDs derived from it);
    - it compiles as `lib_6_9` with internal default linkage;
    - it stops on groupshared memory, group barriers or `gl_NumWorkGroups`.
    `--ser` gives the GLSL marker `d3d12_ser_reorder(hint, bits)` (path_tracer_rgen.h,
    `#ifdef VKPT_D3D12`, so Vulkan never sees it) the body `dx::MaybeReorderThread(hint,
    bits)`. cmake's `compile_shader_d3d12_variant(... ENTRY raygen|ser)` builds
    `indirect_lighting.rgen.query[.spec_0_1].{raygen,ser}.dxil`.
  - The hint (indirect_lighting.rgen, right after the bounce's `get_hit_triangle`): the
    hit's material kind in bits 12-15 and its index in 0-11, 16 bits. The shader has no
    wave operations, so the regrouping cannot change a pixel's own result.
  - Dispatch (`passes.c`'s `trace_raygen`): `bindings.c`'s `d3d12_create_raygen_object`
    builds a state object (DXIL library, the path tracer root signature as the global one,
    a 4-byte payload, recursion 1, no hit groups). Its 32-byte identifier sits in a
    64-byte record in an upload buffer. `SetPipelineState1`, then the same push constants,
    then `DispatchRays` over the compute grid rounded up to whole 8x8 groups, depth 2.
  - Measuring: `profiler 1` draws vkpt's table (THE PROFILER below), with the pt_ser
    mode above it: INDIRECT_LIGHTING is the bounces together, _0 the first, _1 the rest.
    On Vulkan the pipeline path cost +0.45 ms on this
    GPU and SER did not win it back (`rtapi-query-vs-pipeline` memory). `pt_ser 2`
    against 0 shows that cost here, and 1 against 2 shows the reorder's gain.
  - Tests: `d3d12_pso_test` makes all four libraries into state objects (4/4 on the 5070
    Ti, debug layer on). `d3d12_world_test` compares `FLAT_COLOR` across pt_ser 0/2/1 over
    the same frames: built, not run.
  - Not done: `HitObject::FromRayQuery` sorting (the hint is the material only), and SER
    on the other passes (primary rays, reflect/refract, direct lighting).
  - **Measured by Matt in game, 11-06 (5070 Ti):** `pt_ser 0` 10.7 ms, `2` 10.6 ms, `1`
    11.7 ms.
    - The ray generation path costs nothing (2 vs 0). Vulkan's +0.45 ms was `traceRay`
      spilling registers; here the rays stay inline RayQuery.
    - The reorder costs 1 ms more than it gains (1 vs 2). At the reorder point it has to
      save everything the shader holds, and the shading after it is light (base colour
      at mip 2, diffuse), which is too little divergence to win that back.
    - `pt_ser 1` running at all shows the export gave the game SM 6.9.
    - It stays as an A/B at default 0. SER is only worth another look if the bounce gets
      full materials or a multi-bounce loop.
    - 11-07 correction: the bounce already HAS full materials - `pt_bounce_material` 1
      (the default) runs get_material (normal map, roughness, metallic, ray-cone mip) and
      GGX direct light at the hit. So "light shading" is not the reason. What stood out:
      the reorder came AFTER `get_hit_triangle`, so the fetched Triangle (~40 live values)
      went through memory with every lane, and its scattered fetch ran before the regroup.
  - **`pt_ser 3` (11-07; Matt measured it "just about breaks even"):** so the 1 ms loss of
    `pt_ser 1` was the spill, and the regroup itself buys about nothing here. The reorder BEFORE the triangle fetch,
    keyed by the primitive's own material (one load; instance override and animation left
    out). Only the 5-value hit record crosses it, and the fetch runs regrouped.
    indirect_lighting.rgen `spec_ser_early` (constant 1, under `VKPT_D3D12`); CMake
    variants `.spec_1_1.ser` / `.spec_0_1_1_1.ser`; world.c `ser_variant`. A/B 1 vs 3 vs 2.
    The rest of the pass's live state (primary surface, throughput, ReSTIR GI candidate)
    still crosses it - reloading that from the G-buffer after the reorder is the next step
    if 3 helps but not enough.
- **Clustered geometry + partitioned TLAS (Agility 1.722 preview, RTX 50 driver):**
  - This is what the wccftech "220x" story measured: acceleration structure *build* speed
    on heavily animated, dense geometry in a synthetic test, not a game's frame rate.
  - In Quake II the win is in rebuilding animated model BLAS every frame (players, monsters,
    gibs, Flow) and in TLAS updates on busy maps.
  - Vulkan has the same hardware path through `VK_NV_cluster_acceleration_structure` and
    `VK_NV_partitioned_acceleration_structure`. Both renderers have both since 11-06
    (`pt_clusters`, `pt_ptlas`).
- **Work graphs 1.0 (retail):** GPU-driven pass scheduling. It has a chance on the many small
  froxel/fog/Flow dispatches.
- **Enhanced barriers, GPU upload heap:** fewer state transitions, and per-frame uploads
  written straight into VRAM.

### THE PROFILER (11-06)

- **vkpt's rows, vkpt's arithmetic.** The profiler's entry list moved out of vkpt.h into
  `vkpt/profiler.h`, which `passes.h` includes (no Vulkan header). `gpu_timers.c` keeps a
  timestamp query heap of `D3D12_PROFILER_QUERIES` per frame slot, at the same
  `entry * 2 + PROFILER_START/STOP` as vkpt's pool; `d3d12_timer_mark` (passes.c) marks
  each pass where vkpt's BEGIN/END_PERF_MARKER are, in passes.c and world.c. The frame's
  list resolves only the queries written (runs), and R_BeginFrame, after the slot's fence,
  hands the ticks to vkpt's `vkpt_profiler_take_results`, which keeps the averages, draws
  `draw_profiler` and answers `profiler_dump` for both renderers. Written every frame, as
  on Vulkan; `profiler` only draws them.
- **Stamps on the queue:** GPU_FRAME's start (R_BeginFrame) and the Flow row's pair go in
  a small list of their own, executed on the queue (vkpt's `vkpt_profiler_query_after`):
  Flow executes during R_RenderFrame, ahead of the frame's list.
- **Two-piece spans:** the shadow map (static half at the list's head, animated models
  after their BLASes), the BVH update (dynamic BLASes, then the TLASes after the shadow
  map's second half) and Flow's occupancy and light sums are timed apart
  (`D3D12_PROFILER_*` after vkpt's entries) and added to their rows. FRAME_TIME runs from
  the light upload to DLSS (or to tone mapping without it), as on Vulkan.
- `vkpt_profiler_destroy` used to leave its freed sample buffers behind for the next
  renderer; `vkpt_profiler_release` clears them (both renderers' shutdowns).

### 5: the A/B

Same map, same demo, same cvars, and the same shaders by construction. Compare both
backends' per-pass timestamp queries and the whole-frame GPU time (`gpu frame (whole)`):
`profiler 1` draws the same table on both, and `profiler_dump` prints the same rows
(`api=d3d12` on this one). Tests should cover:

- pure path tracing;
- heavy AS rebuild (crowded maps, gibs);
- DLSS RR on.

Report per pass, not just fps.

## Known debts in phase 1

- **Descriptor slot reuse:** an unloaded image's slot gets a null SRV right away, while a
  frame still in flight may sample it. It only happens at map changes, which wait for the
  GPU. Defer the null write through the release list before images unload mid-game.
- **Allocation:** one committed resource per texture; no heap sub-allocation.
- **Formats:** RGBA8 and R16 only; no mips; SDR only.
- **Sampler gap:** `NEAREST_MIPMAP_ANISO` is not anisotropic, because D3D12 has no mag-point
  anisotropic filter. It matters for `pt_nearest 2`.
- **Leftover callbacks:** vkpt cvars' `changed` callbacks survive a switch from `vid_rtx 1` to
  `3`. None fires into Vulkan state today, but a new one could.
- **Stale 2D record (shared with Vulkan):** vkpt's `enqueue_stretch_pic` counts a pic before
  the clip-rect rejection, so a rejected pic leaves the previous frame's record in its slot,
  drawn again. Both renderers show it. Fix it in vkpt/draw.c.

## Known debts in phase 2 (so far)

- **Upload pool:** the world buffer's upload makes one chunk the size of the world, tens of
  MB, and the pool keeps it until it fills. Release oversized chunks after the batch.
- **Simultaneous-access images:** the G-buffer is never transitioned. Passes are ordered by a
  global barrier, as vkpt's `BARRIER_COMPUTE`, and a pixel shader samples an image a compute
  pass just wrote. GPU-based validation accepts it (`d3d12_world_test`), and
  simultaneous-access textures are uncompressed, so nothing else is needed on any vendor.
  The cost: no colour compression on the G-buffer.
- **Rewrites wait for idle:** a G-buffer resize or a `d3d12_show` change calls
  `d3d12_wait_idle` before rewriting the sets in place. That is fine for rare events, which
  is all they are since 11-06: a DRS step or photo mode changes no image
  (`render_scale_capacity`).
- **The 3D view covers the screen:** the blit ignores `fd->x/y/width/height`, as vkpt's
  does. The player setup's model view (the only sub-view) puts its rectangle in the
  projection instead (prepare_ubo), and is drawn since 11-06.
- **Flow's GPU time is outside the frame's list** (Flow executes its own lists at its
  flush); the profiler stamps it on the queue around the flush, and GPU_FRAME starts in
  R_BeginFrame, ahead of it. (Decals: vkpt has none either.)
- **Tile lists past 16.7M tiles** fall back to the whole grid (65535 rows of 256), as
  Vulkan does past its group limit: far beyond any screen.
- **Sky loads wait for idle:** a preset change, a skybox (`R_SetSky`) and the terrain grid
  call `d3d12_wait_idle`, then rewrite the textures sets in place. They happen at map load
  or on a cvar change, as vkpt's own `vkDeviceWaitIdle` there.
- **The sky's barriers:** one global barrier after the sky and after the resolve, and the
  shadow map moves between DEPTH_WRITE and NON_PIXEL_SHADER_RESOURCE around its draw. The
  envmap is a simultaneous-access texture like the G-buffer.
- **The UBO's DLSS switch** was the raw `pt_dlss` cvar (`DLSSMode()`), so a `pt_dlss` left in
  the config sent the interleave and tone mapping down their DLSS branches with no DLSS
  running - on D3D12, and on Vulkan without NGX. vkpt/main.c now sets it only while
  `DLSSEnabled()`. A Vulkan change in shared code: worth an A/B screenshot with DLSS on.
- **Passes are serialised:** one global barrier after every dispatch, where vkpt names the
  images each barrier covers. Correct, possibly slower; measure before refining.
- **Model uploads wait for idle:** a frame that uploads or frees a model flushes the upload
  batch and waits for the GPU before it rewrites `primitive_buffers[]`, as vkpt waits for
  its queue there. It happens at map load and when a model is registered mid-game.
- **Frame generation's set wait:** the frame's whole command list waits for the present
  queue's last copy out of the set it writes (two frames back), not only the part that
  writes it. Normally that copy finished long before. If the present queue is held up
  (DXGI's buffer wait under FIFO), the next frame's rendering waits too. Splitting the
  list at the present sources would remove it, if it ever shows.
- **Per-frame BLASes, one build each:** the model sections' BLASes are rebuilt in full every
  frame, as vkpt rebuilds them (no refit, no reuse). The memory is the frame slot's own and
  grows by half again when a frame needs more, so it is never made per frame.

## Tests (headless, no window, do not need the game)

- `tests/d3d12_caps_probe.c`: runtime load, caps, the 2D root signature and PSO, and that
  every DXIL file is signed.
- `tests/d3d12_draw_test.c`: draws two StretchPic records through the translated shaders with
  draw.c's bindings under GPU-based validation, then checks pixels. It covers the Y flip,
  texel orientation, the sampler index, sRGB and exact edges. Re-run it after touching
  spv2dxil, `global_textures.h` or `stretch_pic.*`.
- `tests/d3d12_mip_test.c` (no GPU): `mipgen.c` checked against what vkpt's blit chain
  and normalize shader give:
  - every sRGB code survives a level;
  - the encode table is exact over 2^20 values;
  - black + white average to sRGB 188;
  - the odd-size weights (5 -> 2: 0.25 / 0.75; 3 -> 1: the centre);
  - R16 averaging;
  - a 600x90 chain runs 10 levels;
  - normals renormalise with z kept as stored.
  Re-run it after touching mipgen.c.
- `tests/d3d12_pso_test.c`: every `*.comp` / `*.query` DXIL, variants included, gets a PSO
  against some generated root signature (59/59), and each graphics pair the renderer makes
  gets its PSO (1/1: world.c's blit). The SER libraries (`*.raygen.dxil`, `*.ser.dxil`)
  become state objects against the path tracer layout (4/4; needs SM 6.9, which the
  test's Agility factory device has). Re-run it after adding a binding to a shader, a set
  to a pipeline layout, or a graphics pipeline.
- `tests/d3d12_world_test.c`: world.c's frame on a two-quad world, under GPU-based
  validation:
  - the world buffer and BLAS/TLAS built the way `d3d12_world_load` builds them, with a
    `geom_opaque`-style instance and model index -1;
  - vkpt's real `primary_rays.rgen.query` over both checkerboard fields: all 2048 pixels
    have the right hit position and material, or miss;
  - the `d3d12_show` blit through the display set: 4096/4096 hit pixels right;
  - then the lit frame: `passes.c`'s recording (the game's own) for 29 frames in six
    configurations that between them run every pass - A-SVGF with one bounce, caustics and
    ReSTIR GI; compositing with two bounces; A-SVGF with no bounce; bloom at
    `bloom_intensity 1` (the rows just above the quads must take their light, the top row
    past the kernel must stay dark); A-SVGF with two bounces
    and SHaRC (`pt_sharc 1`, cleared on its first frame, then the cache read back: keys
    claimed, entries sampled, every resolved value finite); the half-height bounce. With
    `system`, two more run DLSS for real through `ngx.c` (the DLLs in the root given):
    Super Resolution over A-SVGF, and Ray Reconstruction with TAA folded, each 6 frames
    of a 64x32 render into the 128x64 DLSS_OUTPUT with a Halton jitter. Each frame's
    evaluation must succeed and DLSS_OUTPUT must be finite with the quads lit (1.000)
    over the misses (0.006, 0.025). Without `system` NGX cannot start (the Agility
    factory, above), and they are skipped and say so.
    After Super Resolution, frame generation runs on its images through `ngx_dlssg.cpp`
    (build.ps1 compiles it beside the test):
    - the feature with one generated frame, then with MultiFrameCountMax's when the GPU
      has more;
    - two frames each: DLSS_OUTPUT encoded, every generated frame evaluated, all decoded.
    - Checked: every creation and evaluation succeeds; each generated frame is finite and
      lit; DLSS_OUTPUT comes back within 2% of what it was, clamped to [0, 1] as the
      encode clamps it.
    - Not yet run to the end (stopped for low memory on 11-06).
    After the lit configurations, SER (`pt_ser`): "compositing, 2 bounces" for 3 frames
    in the order compute, compute, pt_ser 2, pt_ser 1. The second compute run is the
    reference, since each run after the first starts from the history an identical run
    left. Each run's `FLAT_COLOR` must have a mean within 1% of the reference's and at
    most 1% of its pixels off by more than 1%, and the bit-identical share is printed.
    Different DXIL can contract float math differently and flip a random decision, so
    bit equality is not required. On the system runtime (`system`) it is skipped (SM
    6.8). Built 11-06, not run.
    The UBO is vkpt's cvar defaults plus vkpt_prepare_frame's fields, and a sun is bound
    (the game has none yet). Per configuration: TAA_OUTPUT finite and lit, and the quads'
    HDR colour within 15% of the analytic sun term (0.513 measured against 0.524);
  - phase 3, the sky, with `env=<dir>` holding `q2rtx_media.pkz`'s `env/*.dds` (unzip it:
    a pkz is a zip). Stroggos, with `precomputed_sky_params.h`'s real atmosphere:
    - all eight DDS files parse to the right shape and format (3D, cube, legacy masks);
    - the terrain shadow map (2,093,058 triangles) against a CPU rasterizer with Vulkan's
      rules: 149 of 4.2M texels differ (0.004%, edge ties), against 1.72M for the opposite
      cull, so the y flip and the cull state carry over;
    - the envmap's six layers finite, the sky above (1.23) over the ground below (0.13);
      the resolved sun (luminance 0.37) and sky colours;
    - 6 frames under ENVIRONMENT_DYNAMIC with the sky recorded on frames 0 and 3 (misses
      0.69 against 0.016 without a sky; the quads' HDR 0.106 over the sun's own 0.048), then
      3 under ENVIRONMENT_STATIC with a grey skybox cube;
    - the space sky's PSO made on the physical sky's layout (not drawn);
  - phase 4, `reflect_refract`, on a second world: a chrome mirror (z 100) facing a chrome
    wall behind the camera (z -100). Each mirror pixel's surface after one bounce is where
    its ray lands on the wall, (300 dx, 300 dy, -100), and after two where it lands back on
    the mirror, (500 dx, 500 dy, 100) - all 2048 launches, to within the rgba16f view
    direction's drift (about 1/1000 of the path). The UBO there is TAAU (unjittered) with
    `pt_roughness_override 0`: primary_rays turns chrome rougher than
    `MAX_MIRROR_ROUGHNESS` into a regular surface, and untextured surfaces are pinned at
    roughness 1. Glass and water ride the same dispatch, with the checkerboard field
    choosing reflection or refraction; they are not checked separately. Both bounce
    counts run again over tile lists, in rows of 3 groups (the game's are 256) so that
    the mirror's 8 tiles take three rows and a part-empty last one:
    - PT_SHADING_POSITION must match the full grid's to the bit;
    - each list must hold the mirror's tiles with its row count (lists 0..n), and the
      rest must be empty;
  - the lit UBO zeroes `pt_aperture` as `prepare_ubo` does outside photo mode (it had the
    cvar's 2, so depth of field blurred everything off the 200-unit focal plane);
  - phase 5, dynamic models, recorded in `world.c`'s order:
    - `animate_materials` over a world of two-frame materials: the three primitives under
      `num_static_primitives` step to their next frame, flags kept, and the fourth stays;
    - a two-frame model in a model slot and a ModelInstance drawing it at backlerp 0.25,
      turned a quarter and moved, at a non-zero `render_prim_offset`. `instance_geometry`
      writes all 12 vertices where the CPU puts them, to 1e-3, in both the primitives and
      the positions buffer. Its fp16 motion is the move to `transform_prev` at backlerp 0.5,
      and the instance's material, cluster and alpha are set;
    - `accel.c`'s plan and build over the instanced positions: an empty section gets no
      BLAS, the model's gets one, and the memory is kept from frame to frame. A TLAS of
      phase 1's world and that BLAS is built as `vkpt_pt_append_dynamic_instances` adds it;
    - the lit frame's primary rays: all 2048 launches right, 120 of them on the model at
      (150 dx, 150 dy, 150) with its material;
  - phase 6, the effects TLAS, in `world.c`'s order:
    - a particle, a sprite, a beam and an explosion, one to each quarter of the lower half,
      50 units out. Their streams are copied from one staging slice laid out as
      `transparency.c` lays its frame out;
    - the plan builds the explosion as a run at a non-zero offset, the particle and the
      sprite through the one index list (the sprite from its vertex offset), and the beam
      as an AABB, with an empty entry getting no BLAS;
    - two TLASes in one pool share one scratch;
    - `PT_TRANSPARENT` must show each effect in its own colour (red particle, blue sprite
      from bindless texture 1, green beam, white explosion from material 7's texture 2) and
      nothing anywhere else;
  - phase 7, blood: `d3d12_record_blood_upload` over instanced buffers that hold other
    bytes, in three frames:
    - an epoch frame zeroes the section between its regions (one gap is past the 1 MB
      zero buffer in both buffers) and copies the regions. One region that runs past the
      section's end is skipped, and zeroed;
    - an ordinary frame writes its one region and nothing else;
    - an empty frame records nothing.
    After each frame, every byte of both buffers must match the same frame applied on the
    CPU. A mutant with the zeroing removed fails it: 117700 primitives wrong;
  - phase 8, the fog, over phase 1's world:
    - `d3d12_record_shadow_map` into a two-layer map: quad A, quad B through its own
      buffer view and a matrix that moves it 50 units (an instance), and quad B again by
      its first vertex into the water layer. The depth where each projects must be the
      CPU's (within 0.0003 on 11-06), and 1 where nothing does;
    - the lit frame with no fog, then the classic god rays (the march, the filter), then
      map fog (`fog_mode 2`) on the froxel grid with ReSTIR and a bake batch, both marches
      and the filter. The test's sun sits ahead of the camera: from behind,
      gr_eccentricity's phase function leaves under 1% of the light;
    - PT_TRANSPARENT must be finite, and lit over the misses: 0.0034 for the god rays and
      0.0016 for map fog, against 0 with no fog;
    - the integrated volume finite and filled, and the bake must write its 256 voxels and
      nothing else;
  - 0 validation errors.

  Run it twice: as it is (the Agility runtime, nothing filtered), and with `system` (the
  device as the game makes it under `pt_dlss`; that run counts the system layer's 1358
  apart). The NGX link: `/I %DLSS_SDK2_PATH%/include` and `nvsdk_ngx_s.lib` (the header
  has the line). It takes about 4 minutes with GPU-based validation (232 s on 11-06 with all 8 phases, 250 s with `system`; `nogbv`: seconds). Without `env=`
  phase 3 is skipped, and says so. Re-run it after touching world.c's sets, barriers or blit,
  `passes.c`, `sky.c`, `accel.c`'s per-frame BLASes, the transparency streams, the blood
  upload, the fog's sets or shadow map, or the shaders' hit, effects, lighting, sky, fog and instancing paths (`instance_geometry`, `vertex_buffer.h`).
- `tests/d3d12_sets_test.c`: all 110 G-buffer images, both textures sets, UBO + vertex
  buffer sets, then 4 real vkpt post passes over 2 frames under GPU-based validation
  (0 errors).
- `tests/d3d12_accel_test.c` (+ `rayquery_test.comp`): classic BLAS/TLAS and ray query
  through the translated route, 256/256 rays, including a 48-geometry BLAS whose rays must
  each report their own geometry index. After editing the shader, rebuild it with the
  glslang and spv2dxil lines at the top of the test.
- `tests/d3d12_ptlas_test.c`: the partitioned TLAS over five frames, on NVAPI (or the
  preview runtime with `preview`). It exits 2 (skipped) without the route.
- `tests/d3d12_cluster_test.c` (+ `cluster_test.comp`): the clustered per-frame BLASes
  against the whole ones, 256/256 rays per pass. Exits 2 without NVAPI cluster operations.
  Since 11-06 also the persistent BLAS (the blood's reuse): a third pass on the first
  pass's memory, with B's vertices zeroed, still gets every B hit from the reused BLAS
  at its old address. A plan with B one triangle shorter builds it. Each pass is
  executed and waited for, as a frame slot's fence would. PASS, GBV clean.
- `d3d12_accel_test.c`, `d3d12_ptlas_test.c` and `d3d12_world_test.c` include
  `nvapi_rt.c`, `vkpt/ptlas.c` and `vkpt/clusters.c` beside `accel.c`, which calls them.

Every file says how to build it in its header.
