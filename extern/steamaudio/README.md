# Steam Audio

src/client/sound/steamaudio.c mixes the game's sounds through Steam Audio
(HRTF, occlusion, transmission). This directory holds what the build needs from
it:

- `include/phonon.h`, `include/phonon_version.h`: the C API headers of Steam
  Audio 4.8.1. When they are present, src/CMakeLists.txt defines
  `USE_STEAMAUDIO`. Without them the client still builds, and OpenAL places
  every sound.
- `LICENSE.md`: Steam Audio's licence (Apache 2.0).
- `THIRDPARTY.md`: notices for what is compiled into our phonon.dll.

The runtime, phonon.dll, is not in git. It is loaded at run time from beside
q2rtx.exe. Build it with:

    python setup/build_phonon.py

That builds Steam Audio from its source, at a pinned commit, without Intel IPP
or the other closed or vendor libraries. The release workflows run the same
script. Do not use Valve's prebuilt phonon.dll from the SDK zip: it links Intel
IPP, whose licence does not allow shipping it next to a GPL program.

To move to a new Steam Audio version, update the commits and hashes in
setup/build_phonon.py and run it once with `--update-headers`.
