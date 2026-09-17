# Quake II RTX Overdrive

[![Build Status](https://github.com/mstewart248/Q2RTX-MOD/actions/workflows/build.yml/badge.svg)](https://github.com/mstewart248/Q2RTX-MOD/actions/workflows/build.yml)

**Quake II RTX Overdrive** is a fork of NVIDIA's **Quake II RTX** that takes the
path tracer forward and pairs it with the content and the game logic of the 2023
*Quake II* remaster.

Two things happened to the original project that this fork is built around.
NVIDIA's DLSS grew a denoiser (Ray Reconstruction), a frame generator and a
latency path, none of which Q2RTX ever shipped; and id/Nightdive released a
remaster whose maps, skeletal models, cinematics, navigation data and gameplay
Q2RTX cannot load at all. Overdrive does both: it is a modern DLSS integration,
and it plays the remaster's campaigns with real path-traced lighting instead of
the remaster's raster renderer.

Lineage: **Quake II RTX** builds on [Q2VKPT](http://brechpunkt.de/q2vkpt), which
builds on [Q2PRO](https://skuller.net/q2pro/), which is a modernised *Quake II*.
Most Q2PRO and Q2RTX console variables still work here.

## License

**Quake II RTX Overdrive** is licensed under the terms of the **GPL v.2** (GNU
General Public License). You can find the entire license in the
[license.txt](license.txt) file.

The **Quake II** game data files remain copyrighted and licensed under the
original id Software terms, so you cannot redistribute the pak files from the
original game — or from the remaster.

## Downloads

Functional builds are published from the
[Actions](https://github.com/mstewart248/Q2RTX-MOD/actions) tab. Download the
artifact, extract it, and put `q2rtx_media.pkz`, `blue_noise.pkz` and the
`pak*.pak` files from the original game into `baseq2/`. For the remaster
content, see [Playing the rerelease campaign](#playing-the-rerelease-campaign).

![image](https://github.com/mstewart248/Q2RTX-MOD/blob/master/rerelease/screenshots/quake073.png)
![image](https://github.com/mstewart248/Q2RTX-MOD/blob/master/rerelease/screenshots/quake071.png)
![image](https://github.com/mstewart248/Q2RTX-MOD/blob/master/rerelease/screenshots/quake117.png)
![image](https://github.com/mstewart248/Q2RTX-MOD/blob/master/rerelease/screenshots/quake118.png)

---

# Features

Almost everything below is exposed in the in-game menu as well as on the
console. The renderer settings live under **Video** → **Advanced** and
**Effects**; the fog, blood and gameplay pages are their own submenus.

## Upscaling, frame generation and latency

| Feature | cvar | Notes |
|---|---|---|
| **DLSS Super Resolution** | `pt_dlss` | `-1` ultra-performance, `0` off, `1` performance, `2` balanced, `3` quality, `4` ultra-quality, `5` DLAA. Built against the current NVIDIA DLSS SDK — no patched SDK needed. |
| **DLSS Ray Reconstruction** | `pt_dlss_dldn` | DLSS's own denoiser *and* upscaler in one pass. By default it **replaces** A-SVGF (`pt_dlss_bypass_denoiser 1`), which is what the RR integration guide asks for: RR wants the raw, independent path-traced samples that A-SVGF's temporal accumulation destroys. Turning RR on also switches TAA and the classic denoiser off, and restores your previous settings when you turn it back off. |
| **DLSS Frame Generation** | `pt_dlss_fg` | `0` off, or a multiplier of `2`–`6`. Implemented directly against NGX (`nvsdk_ngx_helpers_dlssg_vk.h`) — **no Streamline dependency**. Pick the multiplier so *base framerate × multiplier ≈ your refresh rate*, so a heavier scene wants a **higher** multiplier, not a lower one. Check the base rate with `pt_dlss_fg_stats 1`. |
| **NVIDIA Reflex** | `pt_reflex` | `0` off, `1` on, `2` on + boost. Implemented over `VK_NV_low_latency2`, so it needs **no SDK at all**. Measured here at 16 ms → 3 ms without frame generation. `pt_reflex_fps_max` is a Reflex-paced frame cap, which is smoother and lower latency than sleeping in the main loop; `pt_dlss_fg_pace_to_refresh 1` caps the *render* rate to refresh ÷ multiplier so the presented rate lands exactly on the display refresh. |

DLSS-specific renderer work that made the above actually look right:

* **Full-resolution reflection and refraction fields** (`pt_dlss_split_fields`,
  default on with DLSS). Q2RTX checkerboards glass and water between a
  reflection and a refraction field. The DLSS-RR integration guide explicitly
  lists checkerboarding as a practice to avoid, and it also feeds DLSS-SR
  inconsistent motion vectors. Costs a second lighting pass over the frame.
* **Guide-buffer selection for split surfaces** (`pt_dlss_guide_field`, and the
  per-material `dlss_guide_field` override). On a glass or water pixel the
  renderer has two layers and DLSS-RR can only be told about one: `0`
  refraction, `1` whichever is brighter, `2` reflection, `3` reflection on glass
  only while water and slime keep the refraction.
* **Mirror guides for chrome models** (`pt_dlss_mirror_guides`) — the viewer
  weapon hands RR the mirror's own normal/albedo/roughness instead of the
  reflected surface's.
* **Reflection/refraction motion vectors** with a bounce guard
  (`pt_dlss_mv_bounce_guard`) so multi-bounce paths stop writing `Inf` into the
  motion buffer.
* **Diffuse and specular hit-distance guides** (`pt_dlss_diff_hitdist`,
  `pt_dlss_spec_hitdist`) — RR was previously sizing its diffuse spatial filter
  with no signal at all.
* **Bloom and tone mapping** reworked for the DLSS output chain.
* An extensive DLSS/G-buffer debug view set: `pt_dlss_debug`,
  `pt_dlss_fg_stats`, `pt_dlss_fg_indicator`, `pt_dlss_fg_compare`.

## Path tracer

* **ReSTIR direct illumination** (`pt_restir`) with temporal reuse, permutation
  sampling, pairwise MIS spatial reuse (`pt_restir_pairwise`), a boiling filter
  (`pt_restir_boiling`), a tunable history length (`pt_restir_m_clamp`) and its
  own debug views. `pt_restir_spatial 0` — temporal only — is the right setting
  under DLSS-RR.
* **Underwater screen warp** (`pt_water_warp`). Unlike the original's
  post-process resample, this bends the *camera rays*, so it costs no sharpness
  through DLSS, has real off-screen geometry to bend in from, and never touches
  the HUD. Amplitude, frequency, speed, fade and the optional breathing zoom are
  all tunable; the defaults are calibrated against the software renderer's
  `D_WarpScreen`.
* **Submerged screen blend** scoped separately from the global one
  (`tm_blend_water_enable`, `tm_blend_water_vignette`).
* **Sky system**: `sky_type` selects between the procedural sky and the map's
  own skybox; `sky_use_map_skybox` controls whether rerelease maps show their
  authored skybox (`1`) or always the procedural one (`0`);
  `sky_map_sun_azimuth` / `sky_map_sun_elevation` place the sun on maps with no
  sun entity; `pt_sky_light_scale` scales the radiance a map skybox casts into
  the level.
* **VRAM reduction** of up to ~3 GB versus upstream, plus `pt_blas_fast_trace` /
  `pt_tlas_fast_trace` build-quality switches and a resizable animated primitive
  buffer (`pt_primbuf`).
* Assorted correctness fixes: reflection/refraction checkerboard interleave,
  glass with multi-bounce reflections, stochastic secondary glass rays
  (`pt_glass_secondary_stochastic`), light transfer onto metallic surfaces, and
  crashes in `vid_restart` with DLSS active.

## Volumetric fog and atmosphere

The remaster authors fog on `worldspawn` — 67 of the 76 in-scope maps set height
fog and 49 also set a plain distance fog. KEX renders that as an analytic
per-pixel tint computed from depth and height: a filter over the framebuffer that
cannot be lit and casts nothing. Overdrive feeds the same authored numbers into
the **density of a real scattering medium** instead.

`cl_fog` selects the integrator. These are four genuinely different things, not
four strengths of one effect, and they do not share their settings — the menu
shows only what the selected mode actually reads:

| `cl_fog` | Name | What it does |
|---|---|---|
| `0` | off | no atmosphere |
| `1` | sunlight only | the classic god-ray march, sun term only, no map lights |
| `2` | lit by map lights | per-pixel march that also evaluates the map's lights, with a hand-rolled falloff model (`pt_fog_light_knee`, `_falloff`, `_pivot`, `_radius`) |
| `3` | volumetric lighting | the **froxel grid** — samples each light properly with its own occlusion ray, so inverse square falls out of the pdf. Ignores the mode-2 falloff knobs entirely. |

**The froxel grid** (`pt_fog_froxel`, the engine behind `cl_fog 3`) is a
160 × 88 × 64 view-frustum-aligned volume — screen-space in X and Y, and
*exponentially* distributed in Z, because a linear slice distribution would spend
most of its cells on distance nobody can resolve. That is ~900k cells against the
tens of millions of light evaluations a per-pixel march costs, and because each
cell is a fixed point in view space, last frame's result can be reprojected into
this one, which is what removes the noise (`pt_fog_froxel_history`,
rate-corrected by `pt_fog_froxel_history_hz` so the *time* constant stays fixed
whatever framerate the machine runs at).

Also in the fog system:

* **ReSTIR for the froxel grid** (`pt_fog_restir`) — a cell borrows which *light*
  its neighbours' candidate draws picked, then still traces its own visibility
  ray on the winner. It costs no extra rays and, unlike the spatial blur, does
  not soften the sky shafts.
* **Real occlusion** (`pt_fog_light_shadow`) so fog is no longer lit *through*
  walls, floors and roofs — this is what makes a glow pool around a fixture
  instead of appearing above the roof it is under. Affordable in the grid (one
  ray per cell); very expensive in the per-pixel march.
* **Per-light volumetric scale**, RTX Remix's `volumetricRadianceScale`. See
  [Volumetric scale](#volumetric-scale-how-much-fog-a-light-makes).
* **Real extinction** (`pt_fog_extinction`, `pt_fog_opacity`) so a long ray
  saturates instead of accumulating without bound, a density ceiling
  (`pt_fog_density_max`), scattering directionality (`pt_fog_eccentricity`), a
  shadowed-ambient term (`pt_fog_ambient`) and soft sky clearance
  (`pt_fog_sky_soften`).
* **Firefly clamping** (`pt_fog_firefly`) — RTX Remix's
  `froxelFireflyFilteringLuminanceThreshold`. Lower this first if the fog looks
  blotchy.
* **Fog in reflections** (`pt_fog_froxel_reflect`) — `0` grid, stopped at the
  reflecting surface; `1` the march's two-pass result, complete but noisy.
* **Fog with DLSS Frame Generation**, which needed its own fixes.
* **Calibration**: `cl_fog_scale` maps the map's authored density onto
  extinction (KEX's constant lives in a closed renderer and cannot be derived).
  `cl_volumetric_fog_density` lets a map carry a *second* density for mode 3,
  because mode 1 zeroes most of the volume behind a hard shadow cliff and needs a
  far larger scale than mode 3, which lights every step. `-1` means "no opinion,
  use `cl_fog_scale`". Set these **per map** with `mapcvar` — see
  [Per-map settings](#per-map-settings-mapcvar) — because `cl_fog_scale` also
  silently sets the march step length and therefore the cost of the pass.
* A large set of `pt_fog_*` diagnostics (`_debug`, `_isolate`, `_log`,
  `_history_clamp`, `_history_hold`, `_const_src`, `_printf`) left in place
  deliberately; they are bisection tools, not settings.

`pt_dlss_fg`, `cl_fog` and `pt_fog_froxel` all interact, so the fog page in the
menu is the reliable way to drive this.

## Quake II (2023 rerelease) support

The remaster's data is loaded from the `rerelease` game directory, straight out
of `pak0.pak` — nothing has to be unpacked. What is supported:

**Maps and geometry**
* All rerelease campaign maps, including the *Call of the Machine* (`mgu*`) set.
  BSP compatibility, mover paths, `func_train` on non-rerelease maps, skyboxes,
  and a long list of brush and lighting oddities across the set.
* `skyautorotate` on `worldspawn`.
* The rerelease's **`dynamic_light` entities** (`cl_dynamic_lights`): 870 of them
  across the 70 non-N64 maps, and they *are* the lighting of those levels. Read
  on the client out of the BSP entity lump and handed to the path tracer as a
  real sphere or spot light — `shadowlightradius`, `shadowlightintensity`,
  `shadowlightconeangle`, `_color`, `shadowlightstyle`, plus an aim entity for
  directional lights. `cl_dynamic_light_scale` and `cl_dynamic_light_cone` tune
  them globally; `cl_dynamic_lights_exclude` drops specific ones.
* **Navigation meshes** (`.nav`, version 6 — all 142 campaign maps) so the
  remaster's monsters path the way they are meant to.
* **Per-map fog** from `worldspawn` — see above.

**Models and animation**
* **MD5 skeletal models** (`cl_md5_models`), falling back to the `.md2` per
  model. `skinnum` keeps selecting the same skin it always did, because the skin
  list is read from the classic model. Normals corrected, and the material system
  learned to carry Q2RTX's hand-authored PBR *tuning* onto the re-unwrapped MD5
  skins — see [MD5 scalar inheritance](#md5-scalar-inheritance).
* `cl_classic_railgun` keeps the classic railgun shape instead of Q2RTX's
  redesigned one.

**Presentation**
* **HD cinematics** (`cl_hd_cinematics`) — the remaster's Theora `.ogv` videos,
  decoded through vendored `libogg` / `libvorbis` / `libtheora`, with audio-clock
  sync, a catch-up budget, a stall watchdog and `cl_hd_cinematics_stats` for
  playback health. The classic 14 fps `.cin` path is untouched.
* **Localized strings** (`g_localize.c`). Rerelease maps store keys, not text —
  `"message" "$map_you_have_found_a_secret"` — and there are 1481 of them across
  827 distinct keys. `localization/loc_english.txt` is used when present
  (including read straight out of `Q2Game.kpf`); otherwise the key is turned back
  into English, which is accurate for effectively all of them. Resolved once at
  spawn, in `ED_NewString`, so it costs nothing at display time.
* **Music playback** from the remaster's `music/` folder alongside the classic
  OGG support.
* **Surface-dependent footsteps** (`cl_footstep_materials`). The remaster ships
  3909 `textures/<name>.mat` files naming one of 15 sound sets — `clank`,
  `glass`, `boot`, `mech`, `wood`, `step`, `splash`, `grass`, `tile`, `carpet`,
  `energy`, `snow`, `junk`, `meat`, `flesh`. The game DLL is not involved, so
  nothing here affects gameplay, prediction, or old demos.
* **Rerelease muzzle flashes** (`cl_muzzleflash_models`) — a starburst model at
  the muzzle rather than only a dynamic light, in first and third person, with
  aim, offset, scale, brightness and duration cvars.
* **`misc_flare` coronas** (`cl_flares`), the rerelease **weapon wheel / weapon
  bar** (`cl_weaponbar`, `cl_weaponbar_hold`, `cl_weaponbar_time`), **boss health
  bars** (`scr_health_bars`), the item-name readout, the flashlight,
  `misc_camera`, the tracker's bubble effect, and lava and emissive surfaces the
  remaster expects to glow.

**Game logic**
* Rerelease monsters and behaviours: **shambler**, **arachnid**, **gunner
  commander**, **guardian**, **stalker**, **widow** and **widow2**, **carrier**,
  **gekk**, **fixbot**, **turret**, soldier variants, and the *Ground Zero* /
  *The Reckoning* monster sets. The rerelease **dodge system**, ledge jumps,
  ground pounds, and a long list of AI fixes — including for the base-game
  monsters that the rerelease changes had broken.
* **Rogue and Xatrix content merged into the single `baseq2` game library**
  (`g_rogue.c`, `g_rogue_items.c`, `g_sphere.c`, `g_xatrix.c`), so *Ground Zero*
  and *The Reckoning* are playable without swapping game DLLs.
* **Campaign selection in the menu**: the main campaign, *Call of the Machine*
  (straight into the hub, no intro cinematic), *The Reckoning* and *Ground Zero*.
* **Weapon switching**: `g_quick_weapon_switch` (the rerelease's 20 Hz raise and
  lower plus `weapons/change.wav`) and `g_instant_weapon_switch`. Neither is
  latched, so the menu toggle takes effect immediately.
* Rerelease items, entities and spawn functions, autosave timing
  (`g_auto_save_min_time`), tracker drag/lift (`g_tracker_drag`,
  `g_tracker_lift`), and the save-game format work to carry all of it.
* Gameplay changes made early in this fork's life are **gated behind menu
  options**, so a fresh install plays with stock behaviour.

## Gore and physical blood

Blood droplets are **real, shaded sphere geometry**, not particles
(`cl_blood_spheres`). Q2RTX particles live in the effects TLAS, where a hit runs
a radial falloff and alpha-composites the result over the already-traced image —
no normal, no BRDF, no secondary ray. A particle therefore cannot be lit, cast a
shadow, or appear in a reflection at any price. These droplets go into the
**geometry** TLAS instead, as ordinary triangles with the full material path:
direct light, indirect bounce, shadows, and a presence in mirrors and water. All
droplets share one BLAS and one TLAS instance with positions baked in world
space, so a burst of sixty costs one instance, not sixty.

* **Simulation**: gravity, drag, speed, stretch, air lifetime, and collision
  against world geometry *and* models (`cl_blood_collision`,
  `cl_blood_model_collision`).
* **Landing and flow**: splats, streaks down walls, runs under gravity across
  sloped surfaces, edge detection so a run reaches a lip and drops off, pooling
  into puddles that grow and merge, draining, and flesh that behaves differently
  from stone (`cl_blood_flesh_cling`, `_damp`, `_run`).
* **Shading**: per-droplet light/dark variation, roughness and specular
  (`pt_blood_roughness`, `pt_blood_specular`), an animated surface ripple
  (`pt_blood_normal_*`, `pt_blood_wobble`), thin-film darkening
  (`pt_blood_thin_dark`, `_thin_power`), splat alpha and darkening, puddle sink.
* **Colour**: `cl_blood_color` forces one colour — red, green, blue, yellow,
  purple, or black/oil — while keeping the per-droplet shade variation, or leaves
  the game's own red/green as sent.
* **Cost controls**: `pt_blood_tess` (20 / 80 / 320 faces per droplet), distance
  LOD (`pt_blood_lod_near`, `_far`, `_air`), `cl_blood_max`, and
  `cl_blood_stats` / `pt_blood_stats`.
* **Sound**: splatter and pooling sounds with their own distance, attenuation,
  spacing and thinning rules — because thinning landing sounds barely dents a
  busy fight, and the pooling rate is the knob that does.
* **Motion vectors** are per-vertex previous-position deltas packed into
  `custom0..2` and tracked client-side per droplet, which is what keeps bursts
  from leaving red comet trails under DLSS-RR.
* **`g_ludicrous_gibs`** (off by default): far more gib chunks, heavier blood
  trails, gibs that never expire, and corpses that can be shot apart in stages
  after they are already dead. **`g_no_janitor`** stops the entity janitor
  removing gibs and corpses.

## Authoring and debugging tools

* **[The light placement system](#the-light-placement-system)** — place, tune and
  delete real path-traced lights from inside the running game, saved to a per-map
  text file.
* **[The expanded material system](#the-material-system)** — roughness and
  metallic maps, RTX-Remix-style texture sidecars, per-material volumetric scale,
  DLSS guide-field overrides, MD5 scalar inheritance, and `mat which` /
  `mat reload`.
* **[`mapcvar`](#per-map-settings-mapcvar)** — set a cvar for the current map
  only, restoring the *player's* value afterwards rather than the compiled-in
  default.
* **`whereis <file>`** and **`path`** to see which archive or directory a file
  actually came from.
* `pt_sun_vis_feedback`, `vid_present_stats`, `vid_swapchain_images`,
  `vid_vsync_mailbox`, `vid_fullscreen_exclusive`, plus Vulkan validation helpers
  (`vk_gpu_av`, `vk_gpu_diag`, `vk_sync_validation`, `vk_shader_printf`).

## Inherited from Quake II RTX

Everything the original project shipped still works: cutting-edge denoising,
cylindrical projection, dynamic "time of day" lighting, caustics and coloured
light through tinted glass, physically based materials, recursive reflections and
refractions on water, glass, mirror and screen surfaces, procedural environments,
sunlight with direct and indirect illumination, volumetric god rays, the player
avatar casting shadows and appearing in reflections, multi-GPU (SLI), deathmatch
and cooperative multiplayer, optional two-bounce indirect illumination,
high-quality screenshot mode, and the [photo mode](#photo-mode).

---

## Playing the rerelease campaign

The rerelease content is loaded from the `rerelease` game directory, and you no
longer have to unpack anything to get it there. Copy the whole `rerelease` folder
out of your *Quake II* (2023 rerelease) install and merge it into this one, so it
sits alongside `baseq2`:

```
Q2RTX/
  baseq2/            <- original Quake II pak files, as usual
  rerelease/         <- copied from <Quake II install>/rerelease
    Q2Game.kpf         (localisation, read straight from the archive)
    baseq2/
      pak0.pak         (all rerelease maps, models, textures and sounds)
      music/
      video/
```

On Steam the source folder is `steamapps/common/Quake 2/rerelease`; the `.exe`
and `.dll` files it contains are ignored and can be left in place or deleted.
Then launch with `+set game rerelease`, or pick the rerelease campaign from the
menu.

Unpacking `pak0.pak` by hand still works exactly as before, and loose files
always take priority over the archives, so anything this project ships in
`rerelease/overrides`, `rerelease/materials`, `rerelease/textures` or
`rerelease/models` continues to override the shipped data either way. Run `path`
in the console to see the resulting search order, and `whereis <file>` to check
which archive or directory a given file came from.

---

# The Light Placement System

A level-design tool rather than a game feature, implemented in
[`src/client/lightedit.c`](src/client/lightedit.c).

The rerelease's own lighting is the `dynamic_light` entities baked into each BSP.
The lights this system places are the same kind of thing — an analytic light
handed to the path tracer — but placed by hand from the console and kept in a
per-map text file instead of in the map. **That is the whole point:** a light can
be placed, judged, retuned and deleted while you are looking at the room it
lights, with no map compile in between.

These lights are deliberately **not** gated by `cl_dynamic_lights`. That cvar
exists to switch the map's own lighting on for comparison, and a hand-placed
light has to stay lit while exactly that comparison is being made. `light_enable`
is their own switch.

## Quick start

```
light debug_on            // draw every light as a sphere you can aim at
light place 255 200 150 200 24 5
                          // warm light, brightness 200, 24-unit emitter, vol 5
light edit brightness 150  // aim at it and retune, one attribute at a time
light print                // read back everything about it
light delete               // aim at it and remove it
```

Every change rewrites `<gamedir>/maps/lights/<mapname>.cfg` immediately, so there
is no save step and nothing to lose on a crash. `light reload` re-reads that file
from disk.

## Commands

| Command | Meaning |
|---|---|
| `light place <r> <g> <b> <brightness> [radius] [volscale]` | put a new light in front of the camera |
| `light replace [<r> <g> <b> <brightness> [radius] [volscale]]` | put a new light in front of the camera that **stands in for** the map light under the crosshair; anything not typed is inherited from that entity |
| `light edit <attribute> <value>` | change one attribute of the light under the crosshair |
| `light edit <r> <g> <b> <brightness> [radius] [volscale]` | the older all-at-once form, still works |
| `light vol <scale \| default>` | shorthand for `light edit vol` |
| `light print` | the light under the crosshair in full, or the whole list |
| `light styles` | enumerate the lightstyles this map has |
| `light delete` | remove the light under the crosshair |
| `light debug_on` / `light debug_off` | show the lights as spheres |
| `light reload` | re-read the file from disk |

`debug_on` and `debug_off` are the only subcommands that mean anything without a
map loaded.

## Attributes

`r`, `g`, `b` and `brightness` are all **0..255, as typed**. `radius` is in world
units and is the size of the emitter sphere, which is what softens the shadow
edge; it also sets the size of the debug marker and of the crosshair pick target,
so what you see in debug mode is exactly what you pick.

`light edit` takes one attribute at a time, so nothing has to be retyped to
change one number:

| Attribute | Value | Aliases |
|---|---|---|
| `rgb` | `<r> <g> <b>`, 0..255 each | `color`, `colour` |
| `red` / `green` / `blue` | one channel of the colour | |
| `brightness` | 0..255 | `bright` |
| `radius` | emitter size in world units | `size` |
| `vol` | volumetric scale | `volume`, `volscale` |
| `cone` | spot cone as a **full** angle in degrees, or `off` for a point light | `angle` |
| `style` | lightstyle index; `0` is steady — see `light styles` | `lightstyle` |
| `aim` | point it where the camera is looking | `direction` |
| `origin` | move it to in front of the camera | `move` |
| `x` / `y` / `z` | nudge one coordinate | |

Every **inheritable** attribute also takes `default`, which unstates it and sends
it back where it came from: the entity for a replacement light, or the plain
default for a placed one. `origin`, `x`, `y` and `z` are the light's own and have
no default. Setting one colour channel of an inherited colour materialises the
other two from the entity first, so `light edit red 255` means what it looks
like.

Every light has all of these — a hand-placed light can be given a cone and a
lightstyle too — and a light that has never been given one behaves exactly as it
always did: a steady point light.

## Volumetric scale: how much fog a light makes

`volscale` is RTX Remix's per-light `volumetricRadianceScale`: how much a light
scatters into the fog **relative to** how much it lights surfaces.

* `1` — the same as its surface lighting
* `0` — lights the room but makes no fog at all
* above `1` — a light that is mostly there for its beam

Omitting it, or `light vol default`, leaves the light on its **class default**,
and that is what the file stores: the column is only written for lights that were
actually given one, so raising the class cvar still moves every light that never
had an opinion.

Nothing in *Quake II*'s data carries such a number, so the resolution order is:

1. the light's **own** value, if it has one (`light place … <volscale>`, a
   `dynamic_light` entity's `volumetric_scale` key)
2. `pt_fog_scale_sky` for **sky brushes** — the fog counts these through its own
   sky term with its own visibility trace, which is why that default is `0`
3. the **material's** `volumetric_scale`, if the surface has one
4. the **class default**:

| Class | cvar | Default | What is in it |
|---|---|---|---|
| dynamic | `pt_fog_scale_dynamic` | `1.0` | every `dlight_t`: weapon fire and muzzle flashes, `misc_flare`, `target_light`, the flashlight, the rerelease `dynamic_light` entities, and the lights placed with `light place` |
| emissive | `pt_fog_scale_emissive` | `1.0` | every triangle of a BSP or model face whose material is `is_light` — with `cl_dynamic_lights 0`, nearly all of a map's lighting |
| model | `pt_fog_scale_model` | `1.0` | emissive surfaces on md2/md5 models, and beam and laser cylinder lights |
| sky | `pt_fog_scale_sky` | `0.0` | sky brushes |

All four are plain (non-archived) cvars, so changing one really does change
behaviour for an existing install.

## Standing in for the map's own lights

The rerelease's `dynamic_light` entities no longer light anything themselves.
They are **dormant**: with debug on they draw as **black** spheres marking a
position, and `light print` on one dumps everything the entity says.

`light replace` then builds a real light that stands in for it — the entity stops
emitting for good and the new light is drawn instead. The replacement is a
**normal light in its own right**, placed in front of the camera rather than at
the entity, so its position is yours to choose. What it takes from the entity is
every attribute you did not type: colour, brightness, cone, lightstyle, aim — and
its **switchability**, so a light the game turns on and off goes on doing that.
Anything the entity does not specify falls back to the ordinary default, which is
what happens with the volumetric scale.

A replaced entity keeps a half-size black marker while debug is on, so a glance
at a room says which map lights have been dealt with. Deleting the replacement
puts the entity back to dormant.

Only debug mode makes a dormant map light pickable — with the markers off there is
nothing on screen to aim at.

## The file

`<gamedir>/maps/lights/<mapname>.cfg`, rewritten in full after every change. It
is read back through the normal search path, so a finished set can be packed into
a `.pkz` and shipped.

```
// placed lights for mgu1m1 - written by "light place"
// x y z   r g b   brightness   radius   [volumetric scale]   [cone d] [style n] [aim x y z]

// lights that STAND IN for the map's own dynamic_light
// entities, which stop emitting. The first origin is the
// ENTITY's, and is the link; the second is this light's own.
// Attributes not listed are inherited from the entity.
// maplight <entity x y z>  <x y z>  [rgb r g b] [bright n] [radius r] [vol v] [cone d] [style n] [aim x y z]
maplight 1072.0 2200.0 -32.0   1071.9 2190.7 -30.9  radius 4.0  vol 5.000
```

A replacement is linked to its entity by the **entity's origin**, stored as its
own column: that is the one identity these entities have, it is readable, and it
survives a recompile as long as the light has not moved. The replacement's own
origin is separate, because it is allowed to be somewhere else entirely.

## cvars

| cvar | Default | Meaning |
|---|---|---|
| `light_enable` | `1` | master switch for hand-placed lights; independent of `cl_dynamic_lights` |
| `light_scale` | `2000` | maps `brightness 255` onto engine light units. The map's own lights come out around 128..2048, so full brightness here is comparable to a bright one of those |
| `light_place_dist` | `64` | how far in front of the eye a new light goes when nothing is in the way |
| `light_pick_size` | `8` | floor on the crosshair pick radius, so a small light is still hittable from across a room |
| `light_debug_brightness` | `1` | brightness of the debug markers. They are emissive particles, so this is the knob for stopping them washing out the room you are judging |
| `light_marker_opacity` | `4` | how solid the map lights' black markers read. This is an alpha but is deliberately allowed **above 1**: a radial falloff is applied and then clamped, so `1` is a soft translucent blob and `4` is a solid disc with a soft rim |

The renderer's whole frame budget is `MAX_DLIGHTS` (256), shared with the map's
own `dynamic_light`s, so at most 256 hand-placed lights are kept per map.

---

# The Material System

The engine defines surface properties — textures, material kinds, flags — in
`*.mat` files. Overdrive keeps the original format and load order and extends it
in several directions.

## Files and load order

1. Every `materials/*.mat` in the game directory (or **directories**, when
   playing a non-base game) is read in alphabetical order, and later files
   override earlier ones.
2. Then `<mapname>.mat` is read when a map loads. Its materials override the
   global ones — but only for **map geometry**, not models.

Loose files always beat archives, so `rerelease/materials/*.mat` shipped by this
project overrides whatever the remaster's `pak0.pak` contains.

An entry can define several materials at once:

```
textures/e1u2/wslt1_5,
textures/e1u2/wslt1_6:
    texture_base overrides/*.tga
    texture_normals overrides/*_n.tga
    texture_emissive overrides/*_light.tga
    is_light 1
```

The `*` in a texture path is replaced with the material's base name, so either
`wslt1_5` or `wslt1_6` above.

## Attributes

Attributes marked **(new)** do not exist in upstream Quake II RTX.

| Attribute | Type | Meaning |
|---|---|---|
| `texture_base` | path | albedo |
| `texture_normals` | path | normal map |
| `texture_emissive` | path | emissive map |
| `texture_mask` | path | alpha / cutout mask |
| `texture_roughness` | path | **(new)** roughness map. Linear data, never sRGB |
| `texture_metallic` | path | **(new)** metallic map. Linear data, never sRGB |
| `kind` | name | `REGULAR`, `CHROME`, `GLASS`, `WATER`, `SLIME`, `LAVA`, `SKY`, `INVISIBLE`, `SCREEN`, `CAMERA`, and — newly namable from a `.mat` — `TRANSPARENT`, `TRANSP_MODEL`, `CHROME_MODEL` |
| `is_light` | bool | this surface emits |
| `light_styles` | bool | honour lightstyles |
| `bsp_radiance` | bool | take radiance from the BSP lightmap |
| `default_radiance` | float | radiance when `SURF_LIGHT` is absent |
| `synth_emissive` | bool | generate an emissive texture from the base texture |
| `emissive_threshold` | int | threshold for that synthesis |
| `emissive_factor` | float | scales the emissive |
| `base_factor` | float | scales the albedo |
| `bump_scale` | float | normal-map strength |
| `roughness_override` | float | forces roughness |
| `metalness_factor` | float | scales the sampled metallic |
| `specular_factor` | float | scales the specular lobe |
| `volumetric_scale` | float | **(new)** how much every surface using this material scatters into the fog, relative to how much it lights the room. Negative (the default) means "not stated", and the class default applies — see [Volumetric scale](#volumetric-scale-how-much-fog-a-light-makes) |
| `curved_water` | bool | **(new)** says this `WATER` / `SLIME` surface is a **closed shape** rather than a flat brush face, so *all* of it takes the force-field (glass) path rather than only the part whose normals face sideways. That is both uniform and the only way a water surface shows its own texture |
| `dlss_guide_field` | 0..3 | **(new)** overrides `pt_dlss_guide_field` for surfaces using this material. Only consulted where the renderer splits a pixel into a reflection and a refraction field — glass, water, slime, chrome — and ignored everywhere else. Same numbering as the cvar, so whichever value looked right in the console is the value to write here |

## Automatic texture discovery

When a material is not defined for a surface — or when a definition only tunes
scalars and does not name a `texture_base` — the engine fills the empty slots
from files sitting next to the base texture. It looks in `overrides/` first, then
in the original texture path:

| Suffix | Slot | Notes |
|---|---|---|
| `_n.tga` | normals | |
| `_rough.tga` | roughness | **(new)** RTX-Remix-style sidecar — no `.mat` entry needed |
| `_metallic.tga` | metallic | **(new)** RTX-Remix-style sidecar |
| `_light.tga` | emissive | |
| `_glow.png` | emissive | **(new)** the rerelease's own name for an emissive map, and it ships one beside nearly every MD5 skin. Accepted **only under `md5/`** — 1571 `_glow.png` files exist across `rerelease/` and `baseq2/` and only 143 are under `md5/`; accepting the suffix everywhere would silently turn hundreds of ordinary wall textures into light sources and relight every map. A `_glow` map masks with alpha, so the alpha is folded into RGB on load |

A slot is only ever auto-filled when the definition did not state it — so an
explicit `texture_normals`, **and an explicit `texture_normals 0`** meaning "this
material has none", wins over whatever happens to be lying beside the texture.

If there is no normal map and no metallic map the material is assumed to be a
plain diffuse one and `specular_factor` / `metalness_factor` are zeroed — unless
the definition asked for those itself.

### Automatic emissive synthesis

With `pt_enable_surface_lights` nonzero, wall surfaces flagged `SURF_LIGHT` (but
not `SURF_SKY` or `SURF_NODRAW`) generate an emissive texture from the base
texture and `pt_surface_lights_threshold`, and are marked `is_light`. For a
material you have defined yourself, use `synth_emissive` and
`emissive_threshold` to ask for this explicitly.

## MD5 scalar inheritance

The rerelease's MD5 models are **re-unwrapped**. Measured across seven models,
the MD5 and MD2 texture coordinates for the same surface point disagree by about
0.6 of the UV range — the same as two unrelated layouts — so none of Q2RTX's
hand-authored PBR *textures* can be reused.

What survives a re-unwrap is everything that is not a texture lookup: how bright
the base is, how rough the surface is, how much the emissive counts. So when an
MD5 skin is loaded, the classic `.md2` skin's material is found and its
**layout-independent tuning** is copied across: `bump_scale`, `emissive_factor`,
`base_factor`, `light_styles`, `bsp_radiance`, `default_radiance`,
`emissive_threshold`, `volumetric_scale`, `dlss_guide_field`, the material
**kind**, and the `curved_water` bit. Textures, and anything that names one, are
deliberately not copied.

Without this an MD5 model renders at `base_factor 1.0` against the classic
model's 1.5–2.5 — visibly darker than the MD2 it replaces.

Two important exceptions:

* Anything the MD5 skin's **own** definition states wins. Writing
  `specular_factor 3` against an md5 skin is a deliberate statement about that
  skin.
* `roughness_override` and `specular_factor` are **not** inherited when the MD5
  skin brought its **own** `_rough.tga` sidecar, and `metalness_factor` is not
  inherited when it brought its own `_metallic.tga`. Every one of the 140 `.mat`
  entries carrying `roughness_override 1` pairs it with `specular_factor 0`, and
  it means "this MD2 has no roughness map, render it fully diffuse". Inheriting
  that onto an MD5 that *does* have a baked roughness map pinned roughness to 1
  and zeroed the specular lobe, silently discarding all 55 baked MD5 roughness
  maps in the tree — the image loaded, bound, sampled, and the result was thrown
  away.

## The `mat` command

Materials can be examined and modified at run time.

| Command | Meaning |
|---|---|
| `mat help` | print usage |
| `mat print [name]` | print the material at the crosshair, or the named one, with every attribute and its current value |
| `mat which` | **(new)** say **where** the current material is defined — file and line number, or "automatically generated" |
| `mat reload` | **(new)** re-read every `.mat` file from disk, re-apply per-map overrides and MD5 inheritance, re-detect sidecars, and rebuild. Auto-generated materials are left alone, and the count of each is reported |
| `mat save <filename> [all] [force]` | write the active materials out. `all` saves everything rather than only the undefined ones; `force` overwrites an existing file |
| `mat <attribute> <value>` | set one attribute of the material at the crosshair |

`mat reload` and `mat <attribute> <value>` rebuild whatever the change actually
touches — some material changes reclassify a mesh as transparent or masked, which
affects the static model BLAS, and emissive changes have to reach the light
lists.

## Per-map settings: `mapcvar`

```
mapcvar <name> <value>
```

Sets a cvar **for the current map only**, remembering what it was so the next map
load puts it back. This is what a per-map `maps/<name>.cfg` actually wants.

The alternative — set the cvar in the map's cfg and `reset` it in
`maps/default.cfg` — restores the cvar's **compiled-in default**, not the
player's value, so a player running `cl_fog 2` everywhere would silently be
dropped to `cl_fog 0` by every map that touched it. `mapcvar` restores what they
actually had.

* A map whose cfg sets it → gets its value.
* The next map, cfg silent → back to the player's value.
* The same cvar set twice in one map's cfg → latched once, at the value the
  player had on entry.
* The player setting it themselves, from the menu or the console → they now own
  it; the map's override is forgotten and the next map load will not put the old
  one back.
* `q2config.cfg` is written at the player's own value, not the map's, so a
  per-map override cannot leak permanently by quitting while stood on that map.

`maps/<mapname>.cfg` is exec'd on map load, immediately after
`maps/default.cfg`. Fog calibration is the main user: see
[`rerelease/maps/lights/`](rerelease/maps/lights) for placed lights, and the
per-map cfgs for the density each map wants.

---

## Additional Information

  * [Client Manual](doc/client.md)
  * [Server Manual](doc/server.md)
  * [Announcement Article](https://www.nvidia.com/en-us/geforce/news/quake-ii-rtx-ray-tracing-vulkan-vkray-geforce-rtx/)
  * [Ray-Tracing Deep Dive](https://www.nvidia.com/en-us/geforce/news/geforce-gtx-dxr-ray-tracing-available-now/)
  * [Path Tracer Overview Video](https://www.youtube.com/watch?v=BOltWXdV2XY)
  * [GDC 2019 Presentation](https://www.gdcvault.com/play/1026185/)

Many source files carry long comments explaining a whole subsystem, and they are
the real documentation for it:

  * [lightedit.c](src/client/lightedit.c) — the light placement system
  * [mapfog.c](src/client/mapfog.c) — the rerelease's per-map fog, and why its
    density scale is not derivable
  * [dynamiclights.c](src/client/dynamiclights.c) — the `dynamic_light` entity,
    measured across every shipped map
  * [footsteps.c](src/client/footsteps.c) — surface-dependent footsteps
  * [g_localize.c](src/baseq2/g_localize.c) — localized map strings
  * [g_nav.c](src/baseq2/g_nav.c) — the `.nav` mesh format, including the two
    things it will catch you out on
  * [blood.c](src/refresh/vkpt/blood.c) — why blood is geometry and not particles
  * [froxel_shared.h](src/refresh/vkpt/shader/froxel_shared.h) — the froxel grid
  * [vertex_buffer.c](src/refresh/vkpt/vertex_buffer.c) — the per-light
    volumetric scale classes
  * [reflex.h](src/refresh/vkpt/reflex.h) — Reflex with no SDK
  * [DLSSG.h](src/refresh/vkpt/DLSSG.h) — the DLSS Frame Generation shim
  * [asvgf.glsl](src/refresh/vkpt/shader/asvgf.glsl) — the denoiser filters
  * [path_tracer.h](src/refresh/vkpt/shader/path_tracer.h) — the path tracer
  * [tone_mapping_histogram.comp](src/refresh/vkpt/shader/tone_mapping_histogram.comp) — tone mapping

## System Requirements

### Operating System

|             | Windows    | Linux        |
|-------------|------------|--------------|
| Min Version | Win 10 x64 | Ubuntu 16.04 |

Only 64-bit builds are supported. A 32-bit configuration produces the dedicated
server only — the DLSS SDK ships no 32-bit library, and the path tracer is what
pulls DLSS in.

DLSS Super Resolution, Ray Reconstruction and Frame Generation require an NVIDIA
RTX GPU and a recent driver. Reflex uses `VK_NV_low_latency2`. Everything else,
including the path tracer and the volumetric fog, runs on any GPU with Vulkan ray
tracing.

### Software

|                                                         | Min Version |
|---------------------------------------------------------|-------------|
| NVIDIA GPU driver <br> https://www.geforce.com/drivers  | 460.82      |
| AMD GPU driver <br> https://www.amd.com/en/support      | 21.1.1      |
| git <br> https://git-scm.com/downloads                  | 2.15        |
| CMake <br> https://cmake.org/download/                  | 3.8         |
| Vulkan SDK <br> https://www.lunarg.com/vulkan-sdk/      | 1.2.162     |
| NVIDIA DLSS SDK <br> https://github.com/NVIDIA/DLSS     | current     |

The DLSS headers and import library are **not** vendored here. Clone
https://github.com/NVIDIA/DLSS and point the `DLSS_SDK2_PATH` environment
variable at it; CMake picks it up and defines `RG_USE_NVIDIA_DLSS`. Without it
the build still succeeds, with DLSS disabled (`CONFIG_USE_DLSS` is turned off and
a warning is printed). CI does this automatically — see
[.github/workflows/build.yml](.github/workflows/build.yml).

Frame Generation goes through NGX directly, so **no Streamline SDK is needed**,
and Reflex goes through a Vulkan extension, so it needs no SDK either.

## Submodules

* [zlib](https://github.com/madler/zlib)
* [curl](https://github.com/curl/curl)
* [SDL2](https://github.com/libsdl-org/SDL)
* [stb](https://github.com/nothings/stb)
* [tinyobjloader-c](https://github.com/syoyo/tinyobjloader-c)
* [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
* [glslang](https://github.com/KhronosGroup/glslang) (optional, see the `CONFIG_BUILD_GLSLANG` CMake option)
* [openal-soft](https://github.com/kcat/openal-soft)
* [ogg](https://github.com/xiph/ogg), [vorbis](https://github.com/xiph/vorbis), [theora](https://github.com/xiph/theora) — for the rerelease's HD cinematics

## Build Instructions

  1. Clone the repository and its submodules from git:

     `git clone --recursive https://github.com/mstewart248/Q2RTX-MOD.git`

  2. Create a build folder named `build` under the repository root
     (`Q2RTX/build`).

     Note: this is required by the shader build rules.

  3. Copy (or create a symbolic link) to the game assets folder (`Q2RTX/baseq2`).

     Note: the asset packages are required for the engine to run. Specifically,
     the `blue_noise.pkz` and `q2rtx_media.pkz` files or their extracted
     contents. The package files can be found in the
     [GitHub releases](https://github.com/NVIDIA/Q2RTX/releases) or in the
     published builds of Quake II RTX.

  4. Set `DLSS_SDK2_PATH` to a clone of https://github.com/NVIDIA/DLSS if you
     want DLSS.

  5. Configure CMake with either the GUI or the command line and point the build
     at the `build` folder created in step 2.

     `cd build`
     `cmake ..`

     **Note**: only 64-bit builds are supported, so make sure to select a 64-bit
     generator during the initial configuration of CMake.

     Note 2: when CMake is configuring `curl`, it will print warnings like
     `Found no *nroff program`. These can be ignored.

  6. Build with Visual Studio on Windows, make on Linux, or the CMake command
     line:

     `cmake --build .`

## Music Playback Support

Music playback from OGG files, if they can be located. Copy the CD tracks into a
`music` folder either next to the executable, or inside the game directory, such
as `baseq2/music`. The files should use one of these two naming schemes:

  - `music/02.ogg` for music copied directly from a game CD;
  - `music/Track02.ogg` for music from the version of Quake II downloaded from
    [GOG](https://www.gog.com/game/quake_ii_quad_damage).

Music playback is enabled when `ogg_enable` is set to 1. Volume is controlled by
`ogg_volume`. Playback controls, such as selecting the track or pausing, are
available through the `ogg` command. The rerelease's own `music/` folder is
picked up the same way.

Music playback support is using code adapted from the
[Yamagi Quake 2](https://www.yamagi.org/quake2/) engine.

## Photo Mode

When a single player game or demo playback is paused, normally with the `pause`
key, the photo mode activates. In this mode, denoisers and some other real-time
rendering approximations are disabled, and the image is produced using
accumulation rendering instead: the engine renders the same frame hundreds or
thousands of times with different noise patterns and averages the results. Once
the image is stable enough, you can save a screenshot.

The photo mode also has a **Depth of Field** effect which computes "true" DoF —
it works correctly through reflections and refractions and has no edge artifacts,
at the price of a lot of noise, so thousands of frames of accumulation are often
needed. Use the mouse wheel and `Shift`/`Ctrl` modifiers: wheel alone adjusts the
focal distance, `Shift+Wheel` adjusts the aperture size, and `Ctrl` makes the
adjustments finer.

Photo mode also has free camera controls. Once paused, use `W/A/S/D` plus `Q/E`
to move up and down; `Shift` is faster and `Ctrl` slower. Move the mouse while
holding the left mouse button to change orientation, hold the right button and
move up or down to zoom, and hold both buttons and move left or right to adjust
camera roll.

Settings are in the game menu, or see `pt_accumulation_rendering`, `pt_dof`,
`pt_aperture`, `pt_freecam` and friends in the [Client Manual](doc/client.md).

## MIDI Controller Support

The console can be remote operated through a UDP connection, which allows
controlling in-game effects from input peripherals such as MIDI controllers. This
is useful for tuning graphics parameters such as the position of the sun, light
intensities, material parameters and filter settings — and it pairs well with the
light placement and material systems above.

You can find a compatible MIDI controller driver
[here](https://github.com/NVIDIA/korgi).

To enable remote access, set the following console variables *before* starting
the game, i.e. in the config file or through the command line:

```
 rcon_password "<password>"
 backdoor "1"
```

Note: the password set here should match the password specified in the korgi
configuration file.

Note 2: enabling the rcon backdoor allows other people to issue console commands
to your game from other computers, so choose a good password.

## Test Model

The engine supports placing a test model in any location, using any MD2, MD3 or
IQM model:

  - To use the material sampling balls model, download the `shader_balls.pkz`
    package from the [Releases](https://github.com/NVIDIA/Q2RTX/releases) page.
    Place or extract that package into your `baseq2` folder.
  - Run the game with the `cl_testmodel` variable set to the path of the test
    model.
  - Use the `puttest` command to place the test model at the current player
    location.
  - Adjust the test model animation speed with `cl_testfps` and its opacity with
    `cl_testalpha`.

## Support and Feedback

  * [GitHub Issue Tracker](https://github.com/mstewart248/Q2RTX-MOD/issues)

For the original Quake II RTX:

  * [GeForce.com Forums](https://forums.geforce.com/default/topic/1119082/geforce-rtx-20-series/quake-ii-rtx-installation-guide/)
  * [Steam Community Hub](https://steamcommunity.com/app/1089130)
  * [NVIDIA/Q2RTX issues](https://github.com/NVIDIA/Q2RTX/issues)
