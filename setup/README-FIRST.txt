================================================================================
 QUAKE II RTX OVERDRIVE
================================================================================

A fork of NVIDIA's Quake II RTX that takes the path tracer forward and pairs it
with the content and game logic of the 2023 Quake II remaster.

This archive contains the engine, the renderer, every shader, all of the fork's
own authored content, and the stock Quake II RTX media package. It does NOT
contain any Quake II game data, because that data is id Software's and cannot be
redistributed. You supply that from your own copy of the game.


--------------------------------------------------------------------------------
 1. WHAT YOU NEED TO ADD
--------------------------------------------------------------------------------

Extract this archive anywhere. You will have a folder containing q2rtx.exe next
to baseq2/ and rerelease/.

(a) ORIGINAL QUAKE II  -  required

    Copy the pak files from your Quake II install into baseq2/ here:

        baseq2/pak0.pak
        baseq2/pak1.pak     (if you have it)
        baseq2/pak2.pak     (if you have it)

    On Steam these are under  steamapps/common/Quake 2/baseq2

(b) THE 2023 REMASTER  -  required for the rerelease campaign

    Copy the WHOLE rerelease folder out of your remaster install and merge it
    into the rerelease folder here, so you end up with:

        rerelease/Q2Game.kpf
        rerelease/baseq2/pak0.pak
        rerelease/baseq2/music/      (optional, see below)
        rerelease/baseq2/video/      (optional, see below)

    On Steam the source folder is  steamapps/common/Quake 2/rerelease
    The .exe and .dll files inside it are ignored; leave or delete them.

    Merge, do not replace. This archive already ships content in
    rerelease/overrides, rerelease/materials, rerelease/textures and
    rerelease/models, and those must survive the copy - they are what this
    project adds. Loose files always take priority over the archives, so the
    overrides win regardless of which order you copy in.

(c) MUSIC AND CINEMATICS  -  optional

    Copy music/ and video/ from the remaster if you want them. They are large
    and purely optional; nothing else depends on them.


--------------------------------------------------------------------------------
 2. RUNNING IT
--------------------------------------------------------------------------------

    q2rtx.exe                          original Quake II campaign
    q2rtx.exe +set game rerelease      the 2023 remaster campaign

You can also pick the rerelease campaign from the in-game menu.

If something does not load, run  path  in the console to see the search order,
and  whereis <file>  to find out which archive or directory a given file came
from.


--------------------------------------------------------------------------------
 3. REQUIREMENTS
--------------------------------------------------------------------------------

A ray tracing capable GPU and a current driver. DLSS Super Resolution, Ray
Reconstruction and Frame Generation need a compatible NVIDIA GPU; the renderer
runs without them.

The DLSS runtime DLLs are included beside q2rtx.exe:

    nvngx_dlss.dll      Super Resolution
    nvngx_dlssd.dll     Ray Reconstruction
    nvngx_dlssg.dll     Frame Generation

Keep them next to the executable.


--------------------------------------------------------------------------------
 4. WHAT IS IN HERE
--------------------------------------------------------------------------------

    q2rtx.exe                   the game
    q2rtxded.exe                dedicated server
    baseq2/gamex86_64.dll       game logic module
    baseq2/q2rtx_media.pkz      stock Quake II RTX art: textures, models, sky
    baseq2/blue_noise.pkz       sampling noise the path tracer needs
    baseq2/shaders.pkz          compiled shaders, packed
    baseq2/shader_vkpt/         the same compiled shaders, loose
    baseq2/                     sky definitions, materials, map configs, sounds
    rerelease/overrides/        the fork's texture override set
    rerelease/models/           PBR maps for the remaster's skeletal models
    rerelease/materials/        material definitions
    rerelease/textures/         additional textures
    rerelease/materialDev/      material authoring sources
    rerelease/maps/             map configuration
    readme.md                   full feature documentation and cvar reference
    changelog.md                upstream Quake II RTX change log
    license.txt, notice.txt     licensing

Quake II RTX Overdrive is licensed under the GPL v2; see license.txt. The Quake
II game data files remain copyrighted by id Software under the original terms
and are not included here.
