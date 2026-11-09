================================================================================
 QUAKE II RTX OVERDRIVE
================================================================================

A fork of NVIDIA's Quake II RTX that takes the path tracer forward and pairs it
with the content and game logic of the 2023 Quake II remaster.

This archive contains the engine, the renderers, every shader and all of the
fork's own authored content. The stock Quake II RTX media package comes in a
second archive (see 1a below). Neither contains any Quake II game data, because
that data is id Software's and cannot be redistributed. You supply that from
your own copy of the game.


--------------------------------------------------------------------------------
 1. WHAT YOU NEED TO ADD
--------------------------------------------------------------------------------

Extract this archive anywhere. You will have a folder containing q2rtx.exe next
to baseq2/ and rerelease/.

(a) THE STOCK QUAKE II RTX MEDIA  -  required

    A release comes as two zips, because GitHub limits one download to 2 GB:

        Q2RTX-Overdrive-<version>-win64.zip    this one
        Q2RTX-Overdrive-<version>-media.zip    the stock Quake II RTX media

    Extract the -media zip into the same place as this one. Both hold a
    Q2RTX-Overdrive folder, so they merge, and it adds two files:

        baseq2/q2rtx_media.pkz
        baseq2/blue_noise.pkz

    They are the same in every release. If you have them from an earlier
    one, copy them over instead of downloading the -media zip again.

(b) ORIGINAL QUAKE II  -  required

    Copy the pak files from your Quake II install into baseq2/ here:

        baseq2/pak0.pak
        baseq2/pak1.pak     (if you have it)
        baseq2/pak2.pak     (if you have it)

    On Steam these are under  steamapps/common/Quake 2/baseq2

(c) THE 2023 REMASTER  -  required for the rerelease campaign

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

(d) MUSIC AND CINEMATICS  -  optional

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

The NVIDIA DLSS runtime DLLs are NOT included. To use DLSS, supply your own
copies and put them in the same folder as q2rtx.exe:

    nvngx_dlss.dll      Super Resolution
    nvngx_dlssd.dll     Ray Reconstruction
    nvngx_dlssg.dll     Frame Generation

Without them the game runs normally, and the menu does not show the DLSS
options whose DLL is missing. The same DLLs serve both RTX renderers.

THE DIRECT3D 12 RENDERER is around 20% faster than the Vulkan one on the
developer's machine. Pick "rtx (direct3d 12)" under options -> graphics
settings -> renderer..., or type  vid_rtx 3  in the console.

On Windows 11 it runs as is. On Windows 10 it needs one DLL from Microsoft
that cannot be included here, the DirectX 12 Agility SDK runtime:

    1. Download version 1.620.1 of the Microsoft.Direct3D.D3D12 package
       ("Download package" on the page):
           https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.620.1
    2. The file is a .nupkg. Rename it to .zip and open it.
    3. Copy  build/native/bin/x64/D3D12Core.dll  from it into a folder named
       D3D12 next to q2rtx.exe:

           D3D12/D3D12Core.dll

It has to be 1.620.1, the version the game is built against. Without it, on
Windows 10, the game says so in the console and stays on Vulkan.


--------------------------------------------------------------------------------
 4. WHAT IS IN HERE
--------------------------------------------------------------------------------

    q2rtx.exe                   the game
    q2rtxded.exe                dedicated server
    phonon.dll                  Steam Audio: 3D sound and occlusion
    nvflow.dll, nvflowext.dll   NVIDIA Flow: simulated fire and smoke
    baseq2/gamex86_64.dll       game logic module
    baseq2/q2rtx_media.pkz      -media zip: stock art, textures, models, sky
    baseq2/blue_noise.pkz       -media zip: sampling noise the path tracer needs
    baseq2/shaders.pkz          compiled shaders, packed
    baseq2/shader_vkpt/         the same compiled shaders, loose
    baseq2/shader_d3d12/        compiled shaders for the Direct3D 12 renderer
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
    licenses/steam-audio/       Steam Audio's licence and third-party notices
    licenses/nvidia-flow/       NVIDIA Flow's licence

Quake II RTX Overdrive is licensed under the GPL v2; see license.txt. The Quake
II game data files remain copyrighted by id Software under the original terms
and are not included here.
