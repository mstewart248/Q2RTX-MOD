#!/usr/bin/env python3
"""
Build phonon.dll - the Steam Audio runtime that src/client/sound/steamaudio.c
loads - from source, WITHOUT Intel IPP.

    python setup/build_phonon.py

Run from the repository root, in an x64 Visual Studio developer prompt (or
anywhere CMake can find Visual Studio on its own, as on the CI runners).

WHY FROM SOURCE. Valve's prebuilt phonon.dll links Intel IPP, which is under
Intel's own licence rather than an open-source one, and it cannot ship next to
a GPL program. Steam Audio itself is Apache-2.0. With IPP switched off it uses
its own vector maths and PFFFT for the FFTs, so this build holds only open
source: Steam Audio, PFFFT, libmysofa, zlib and the FlatBuffers headers.
Their notices are in extern/steamaudio/THIRDPARTY.md, which the release
package carries.

WHAT IT DOES:

  * downloads the pinned Steam Audio commit (tag v4.8.1) and the commits of its
    dependencies that Valve's core/build/dependencies.json names for that
    release, and checks every archive against the SHA-256 below;
  * builds the dependencies as static libraries on the static C runtime (/MT,
    which is what phonon itself uses), and stages them where Steam Audio's
    Find*.cmake modules look (core/deps/...);
  * patches the Steam Audio source (see PATCHES): its own vector maths, which
    Valve's IPP builds never run, where it is wrong; the reflection response,
    which it builds as one coherent signal and cuts short for a far source;
    and pathing, which leaves stale
    results where no probe is in reach, keeps a path a door has shut, and
    counts a partial result's share twice; a hook that lets the game's
    own GPU tracer stand in for the reflection simulator; and a reflection
    convolution that does no work while its input has long been silent;
  * builds phonon with IPP, MKL, Embree, Radeon Rays and TrueAudio Next off,
    and without its tests, samples and docs.

OUTPUTS:

  * phonon.dll at the repository root, next to q2rtx.exe (gitignored as *.dll;
    setup/package_release.py ships it).
  * extern/steamaudio/include/phonon.h and phonon_version.h, written when
    absent. When they are present they must match this version, or the script
    stops: compiling against one version and loading another is how a struct
    layout silently goes wrong. Pass --update-headers after a version bump.

Work files go to build-phonon/ (gitignored); a second run reuses the downloads
and the dependency builds. The libmysofa archive is ~200 MB because it carries
its test data - CI caches the finished DLL so that is a one-off.
"""

import argparse
import ctypes
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

STEAMAUDIO_VERSION = '4.8.1'

# name: (GitHub repository, commit, SHA-256 of the GitHub archive of that commit)
SOURCES = {
    'steam-audio': ('ValveSoftware/steam-audio', '0da18255cca520771f363ee01f100572b39a308e',
                    'e769e43ea2392a7540b6b50588a710d7c674c975a30a8a80a1af85b4d32bd90f'),
    'flatbuffers': ('google/flatbuffers', '6df40a2471737b27271bdd9b900ab5f3aec746c7',
                    '4aa372a30703e1801514c95281966bbd7c8fb4029ab73af0d911d43ef1d3ab11'),
    'pffft':       ('marton78/pffft', 'e0bf595c98ded55cc457a371c1b29c8cab552628',
                    'd80286fbbe128fcad14140d7c77e27d063c2388a898c130f7c2d63c25b55cdcd'),
    'zlib':        ('madler/zlib', '51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf',
                    'd9e270d46252734aa49770fbc544125391617956266f220bd63216c834f3a522'),
    'mysofa':      ('hoene/libmysofa', 'dd315a8ec1fee7193d40e4a59b12c5590a4a918c',
                    '39362f7caa3e2e3cf89d24d1c1396eff1b4edc01223e6e967f2232db8fb38fa6'),
}

# Where Steam Audio's Find*.cmake modules look (get_bin_subdir / get_host_bin_subdir).
BIN_SUBDIR = 'windows-x64'

# Everything a dependency is built with. CMAKE_POLICY_VERSION_MINIMUM lets
# CMake 4 configure the older dependency projects, which ask for CMake 2.x;
# CMP0091 NEW makes CMAKE_MSVC_RUNTIME_LIBRARY apply to them as well.
COMMON = [
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
]

HEADERS = ['phonon.h', 'phonon_version.h']

# Fixes to the Steam Audio source: (file under core/, text, replacement, times
# the text occurs). They live in this script, not in .patch files, so CI's
# cache of the DLL - keyed on this file's hash - is rebuilt when they change.
PATCHES = [
    # The complex multiply-accumulate's unaligned branch loads the accumulator
    # with an ALIGNED load. A convolution's per-channel spectra are rows of
    # 513 complex values, 4104 bytes, so every odd channel of a multi-channel
    # IR starts 8 bytes off and the first reflection effect applied faults.
    # With IPP this function is never used; without it every reflection is.
    # The aligned branch's identical line is replaced too, harmlessly.
    ('src/core/array_math.cpp',
     'y = float4::add(y, float4::load(&outData[i]));',
     'y = float4::add(y, float4::loadu(&outData[i]));', 2),

    # A reflection response is rebuilt from the simulation's energy field -
    # energy per time bin, band and Ambisonics channel - as band-filtered
    # noise. Valve's reconstruction uses the SAME noise for every channel,
    # scaled by the channel's share, so the response is one coherent signal
    # with an average direction. Late in a room, sound arrives from all round,
    # the directional channels cancel to nothing, and what is left is one
    # noise in both ears through the decoder's fixed filters: a comb-coloured,
    # metallic tail inside the head, whatever the walls are. And where the
    # energy leans one way the whole tail is a single phantom there, loud
    # enough to pull a sound's direction after it.
    #
    # At first order this builds the field instead from six sources of noise,
    # one along each way of each axis, each noise its own (the same sequence
    # a sixth of its length on), each as loud as the energy field says sound
    # arrives from that way. W keeps Valve's energy exactly; a diffuse tail
    # comes out diffuse, with X, Y and Z as strong as W and all four
    # uncorrelated, as in a real room.
    ('src/core/reconstructor.cpp',
     '                    if (type == ReconstructionType::Linear)\n'
     '                    {\n'
     '                        auto energy = 0.0f;\n',
     '                    if (type == ReconstructionType::Linear && numChannels == 4)\n'
     '                    {\n'
     '                        // q2rtx: six incoherent sources, see setup/build_phonon.py\n'
     '                        const auto kSqrt3 = 1.7320508f;\n'
     '                        auto envelopes = [&](int bin, float* env)\n'
     '                        {\n'
     '                            for (auto k = 0; k < 6; ++k)\n'
     '                                env[k] = 0.0f;\n'
     '\n'
     '                            auto e0 = energyField[0][iBand][bin];\n'
     '                            if (e0 < kEnergyThreshold)\n'
     '                                return;\n'
     '\n'
     '                            // the energy arriving from +axis and -axis of each channel\n'
     '                            float q[6];\n'
     '                            auto sum = 0.0f;\n'
     '                            for (auto c = 1; c < 4; ++c)\n'
     '                            {\n'
     '                                q[2 * c - 2] = fmaxf(e0 + kSqrt3 * energyField[c][iBand][bin], 0.0f);\n'
     '                                q[2 * c - 1] = fmaxf(e0 - kSqrt3 * energyField[c][iBand][bin], 0.0f);\n'
     '                                sum += q[2 * c - 2] + q[2 * c - 1];\n'
     '                            }\n'
     '\n'
     '                            // W\'s amplitude squared as Valve has it, shared out\n'
     '                            for (auto k = 0; k < 6; ++k)\n'
     '                                env[k] = sqrtf(e0 / sqrtf(4.0f * Math::kPi) * q[k] / sum);\n'
     '                        };\n'
     '\n'
     '                        float env[6], prevEnv[6];\n'
     '                        envelopes(iBin, env);\n'
     '                        if (iBin == 0)\n'
     '                        {\n'
     '                            for (auto k = 0; k < 6; ++k)\n'
     '                                prevEnv[k] = env[k];\n'
     '                        }\n'
     '                        else\n'
     '                        {\n'
     '                            envelopes(iBin - 1, prevEnv);\n'
     '                        }\n'
     '\n'
     '                        // W hears all six; each directional channel the two on its axis, as\n'
     '                        // plane waves from there encode: +-sqrt(3) times W\n'
     '                        float gain[6] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };\n'
     '                        auto k0 = 0, k1 = 6;\n'
     '                        if (iChannel > 0)\n'
     '                        {\n'
     '                            k0 = 2 * iChannel - 2;\n'
     '                            k1 = k0 + 2;\n'
     '                            gain[k0] = kSqrt3;\n'
     '                            gain[k0 + 1] = -kSqrt3;\n'
     '                        }\n'
     '\n'
     '                        const auto numNoise = static_cast<int>(mWhiteNoise.size(1));\n'
     '                        auto* out = &mBandIRs[iBand][iBin * numSamplesPerBin];\n'
     '                        for (auto iBinSample = 0; iBinSample < numBinSamples; ++iBinSample)\n'
     '                        {\n'
     '                            out[iBinSample] = 0.0f;\n'
     '                        }\n'
     '\n'
     '                        // past the end of the tail: nothing to make\n'
     '                        for (auto k = k0; k < k1; ++k)\n'
     '                        {\n'
     '                            if (prevEnv[k] <= 0.0f && env[k] <= 0.0f)\n'
     '                                continue;\n'
     '\n'
     '                            const auto* noise = &mWhiteNoise[iBand][0];\n'
     '                            auto a = gain[k] * prevEnv[k];\n'
     '                            auto d = gain[k] * (env[k] - prevEnv[k]) / static_cast<float>(numSamplesPerBin);\n'
     '                            auto j = (iBin * numSamplesPerBin + k * (numNoise / 6)) % numNoise;\n'
     '                            for (auto s = 0; s < numBinSamples; )\n'
     '                            {\n'
     '                                // up to where the noise wraps round, in one plain loop\n'
     '                                auto run = std::min(numBinSamples - s, numNoise - j);\n'
     '                                for (auto t = 0; t < run; ++t)\n'
     '                                {\n'
     '                                    out[s + t] += (a + d * static_cast<float>(s + t)) * noise[j + t];\n'
     '                                }\n'
     '                                s += run;\n'
     '                                j = 0;\n'
     '                            }\n'
     '                        }\n'
     '                    }\n'
     '                    else if (type == ReconstructionType::Linear)\n'
     '                    {\n'
     '                        auto energy = 0.0f;\n', 1),

    # Where the response ends. Valve's reconstruction leaves out every bin under
    # kEnergyThreshold, a fixed energy, whatever the source's level - and the
    # energy field is in proportion to that level, falling as the source gets
    # farther. A soldier 25 m off in mgu6m1's canyon starts 75 times quieter
    # than a sound at the listener, so his tail reached the threshold at 460
    # ms and stopped dead while still only 30 dB down: the more a material
    # absorbs, the sooner. The end is put instead 60 dB under the band's
    # loudest bin, the range a reverberation time is measured over, so a far
    # source's response fades out as a near one's does.
    ('src/core/reconstructor.cpp',
     '        numSamples = std::min({ impulseResponse.numSamples(), numSamples });\n'
     '        auto numBins = std::min({ energyField.numBins(), ',
     '        numSamples = std::min({ impulseResponse.numSamples(), numSamples });\n'
     '\n'
     '        // q2rtx: where the tail ends, per band, see setup/build_phonon.py\n'
     '        float tailFloor[Bands::kNumBands];\n'
     '        for (auto iBand = 0; iBand < Bands::kNumBands; ++iBand)\n'
     '        {\n'
     '            auto peak = 0.0f;\n'
     '            for (auto iBin = 0; iBin < energyField.numBins(); ++iBin)\n'
     '                peak = std::max(peak, energyField[0][iBand][iBin]);\n'
     '            tailFloor[iBand] = peak * 1e-6f;\n'
     '        }\n'
     '\n'
     '        auto numBins = std::min({ energyField.numBins(), ', 1),
    ('src/core/reconstructor.cpp',
     '                            if (e0 < kEnergyThreshold)\n',
     '                            if (e0 <= tailFloor[iBand])\n', 1),

    # Pathing with no baked probe near the source or the listener returns
    # without writing its outputs, so a source keeps the paths of wherever it
    # last had a probe in reach, and a new one reads what was there before. Its
    # outputs are zeroed instead - EQ all 0, which a real result never is - so
    # the engine can tell "no probe here" from "no way round" (EQ 1, SH 0).
    ('src/core/path_simulator.cpp',
     '        else\n'
     '        {\n'
     '            return false;\n'
     '        }\n'
     '    }\n'
     '    else\n'
     '    {\n'
     '        SoundPath directPath;\n',
     '        else\n'
     '        {\n'
     '            // q2rtx: no probe in reach - zeroed, see setup/build_phonon.py\n'
     '            if (eqGains)\n'
     '                memset(eqGains, 0, Bands::kNumBands * sizeof(float));\n'
     '            if (coeffs)\n'
     '                memset(coeffs, 0, SphericalHarmonics::numCoeffsForOrder(order) * sizeof(float));\n'
     '\n'
     '            return false;\n'
     '        }\n'
     '    }\n'
     '    else\n'
     '    {\n'
     '        SoundPath directPath;\n', 1),

    # Validation checks a baked path against what moves - a door that has
    # shut - but finding it blocked only matters with alternate paths on,
    # which search for another way; without them the blocked path is used as
    # it was baked. Dropped instead, so validation alone shuts a way off.
    ('src/core/path_simulator.cpp',
     '            if (findAlternatePaths)\n'
     '            {\n'
     '                tryRealTime = true;\n'
     '            }\n'
     '        }\n'
     '    }\n'
     '\n'
     '    if (tryRealTime)\n'
     '    {\n'
     '        ProbePath probePath;\n',
     '            if (findAlternatePaths)\n'
     '            {\n'
     '                tryRealTime = true;\n'
     '            }\n'
     '            else\n'
     '            {\n'
     '                // q2rtx: blocked and no other way looked for - see setup/build_phonon.py\n'
     '                soundPath = SoundPath();\n'
     '            }\n'
     '        }\n'
     '    }\n'
     '\n'
     '    if (tryRealTime)\n'
     '    {\n'
     '        ProbePath probePath;\n', 1),

    # A path's level (the SH) and its EQ are both summed over the ways found,
    # each weighted by its listener probe's share, so where only some of the
    # probes round the listener have a way, both come out short by that
    # share and the two multiplied count it twice. The EQ is made the
    # average of the ways found instead; the level alone carries the share.
    ('src/core/path_simulator.cpp',
     '            ++numValidPaths;\n'
     '        }\n'
     '\n'
     '        if (numValidPaths == 0)\n',
     '            ++numValidPaths;\n'
     '        }\n'
     '\n'
     '        // q2rtx: the average of the ways found - see setup/build_phonon.py\n'
     '        auto validWeight = 0.0f;\n'
     '        for (auto i = 0; i < numPaths; ++i)\n'
     '        {\n'
     '            if (paths[i].isValid())\n'
     '                validWeight += weights[i];\n'
     '        }\n'
     '        if (validWeight > 0.0f)\n'
     '        {\n'
     '            for (auto j = 0; j < Bands::kNumBands; ++j)\n'
     '                eqGains[j] /= validWeight;\n'
     '        }\n'
     '\n'
     '        if (numValidPaths == 0)\n', 1),

    # A tracer of the game's own - steamaudio.c's, on the GPU - can stand in
    # for the real-time reflection simulator. It is given the sources' and the
    # listener's origins and the run's settings, and fills each source's
    # energy field ([channel][band][bin], zeroed first) the way the CPU
    # simulator would (phase 0); when it returns 0 the CPU runs as before, and
    # the tracer is called again with what the CPU made (phase 1), which is
    # how sa_reflcheck compares the two. Everything after - the averaging of
    # runs, the reconstruction, the convolution - is Steam Audio's either way.
    # iplq2SetReflectionTracer hands it over.
    ('src/core/simulation_manager.h',
     'public:\n'
     '    static bool sEnableProbeCachingForMissingProbes;\n',
     'public:\n'
     '    // q2rtx: a tracer of the game\'s own - see setup/build_phonon.py\n'
     '    typedef int (*Q2ReflectionTracer)(int phase, int numSources, const float* sourceOrigins, const float* listenerOrigin,\n'
     '                                      int numRays, int numBounces, float duration, int order,\n'
     '                                      float irradianceMinDistance, int numChannels, int numBins,\n'
     '                                      float* const* energyFields, void* userData);\n'
     '    static Q2ReflectionTracer sQ2Tracer;\n'
     '    static void* sQ2TracerUserData;\n'
     '\n'
     '    static bool sEnableProbeCachingForMissingProbes;\n', 1),

    ('src/core/simulation_manager.cpp',
     'bool SimulationManager::sEnableProbeCachingForMissingProbes = false;\n'
     '\n'
     'SimulationManager::SimulationManager(',
     'bool SimulationManager::sEnableProbeCachingForMissingProbes = false;\n'
     '\n'
     '// q2rtx: the game\'s own tracer, and how it is handed over (NULL takes it back) - see setup/build_phonon.py\n'
     'SimulationManager::Q2ReflectionTracer SimulationManager::sQ2Tracer = nullptr;\n'
     'void* SimulationManager::sQ2TracerUserData = nullptr;\n'
     '\n'
     'extern "C"\n'
     '#if defined(_WIN32)\n'
     '__declspec(dllexport)\n'
     '#else\n'
     '__attribute__((visibility("default")))\n'
     '#endif\n'
     'void iplq2SetReflectionTracer(SimulationManager::Q2ReflectionTracer tracer, void* userData)\n'
     '{\n'
     '    SimulationManager::sQ2Tracer = tracer;\n'
     '    SimulationManager::sQ2TracerUserData = userData;\n'
     '}\n'
     '\n'
     'SimulationManager::SimulationManager(', 1),

    ('src/core/simulation_manager.cpp',
     '    mJobGraph.reset();\n'
     '\n'
     '    mReflectionSimulator->simulate(*mScene, static_cast<int>(mRealTimeSources.size()), mRealTimeSources.data(), 1, &mSharedData->reflection.listener,\n'
     '                                   mRealTimeDirectivities.data(), mSharedData->reflection.numRays, mSharedData->reflection.numBounces,\n'
     '                                   mSharedData->reflection.duration, mSharedData->reflection.order, mSharedData->reflection.irradianceMinDistance,\n'
     '                                   mRealTimeEnergyFields.data(), mJobGraph);\n'
     '\n'
     '    mThreadPool->process(mJobGraph);\n',
     '    // q2rtx: the game\'s own tracer, when it has one and it can - see setup/build_phonon.py\n'
     '    auto q2Tracer = sQ2Tracer;\n'
     '    auto numSources = static_cast<int>(mRealTimeSources.size());\n'
     '    vector<float> q2Origins(3 * numSources);\n'
     '    vector<float*> q2Fields(numSources);\n'
     '    const auto& q2ListenerOrigin = mSharedData->reflection.listener.origin;\n'
     '    float q2Listener[3] = { q2ListenerOrigin.x(), q2ListenerOrigin.y(), q2ListenerOrigin.z() };\n'
     '    for (auto i = 0; i < numSources; ++i)\n'
     '    {\n'
     '        q2Origins[3 * i + 0] = mRealTimeSources[i].origin.x();\n'
     '        q2Origins[3 * i + 1] = mRealTimeSources[i].origin.y();\n'
     '        q2Origins[3 * i + 2] = mRealTimeSources[i].origin.z();\n'
     '        q2Fields[i] = mRealTimeEnergyFields[i]->flatData();\n'
     '    }\n'
     '    auto q2Trace = [&](int phase)\n'
     '    {\n'
     '        return q2Tracer(phase, numSources, q2Origins.data(), q2Listener, mSharedData->reflection.numRays,\n'
     '                        mSharedData->reflection.numBounces, mSharedData->reflection.duration,\n'
     '                        mSharedData->reflection.order, mSharedData->reflection.irradianceMinDistance,\n'
     '                        mRealTimeEnergyFields[0]->numChannels(), mRealTimeEnergyFields[0]->numBins(),\n'
     '                        q2Fields.data(), sQ2TracerUserData) != 0;\n'
     '    };\n'
     '\n'
     '    auto traced = false;\n'
     '    if (q2Tracer)\n'
     '    {\n'
     '        for (auto i = 0; i < numSources; ++i)\n'
     '            mRealTimeEnergyFields[i]->reset();\n'
     '        traced = q2Trace(0);\n'
     '    }\n'
     '\n'
     '    if (!traced)\n'
     '    {\n'
     '        mJobGraph.reset();\n'
     '\n'
     '        mReflectionSimulator->simulate(*mScene, static_cast<int>(mRealTimeSources.size()), mRealTimeSources.data(), 1, &mSharedData->reflection.listener,\n'
     '                                       mRealTimeDirectivities.data(), mSharedData->reflection.numRays, mSharedData->reflection.numBounces,\n'
     '                                       mSharedData->reflection.duration, mSharedData->reflection.order, mSharedData->reflection.irradianceMinDistance,\n'
     '                                       mRealTimeEnergyFields.data(), mJobGraph);\n'
     '\n'
     '        mThreadPool->process(mJobGraph);\n'
     '\n'
     '        if (q2Tracer)\n'
     '            q2Trace(1);\n'
     '    }\n', 1),

    # A reflection effect convolves its input with the whole response every
    # frame - 1.5 s of it, four channels - silent or not; with a new response
    # in, twice. The game gives every source of sound one, and a slot follows
    # a monster for seconds after it falls quiet, so most frames most of them
    # convolve nothing but silence, at a quarter of a millisecond each.
    #
    # Once the input has been silent for as long as the response is long, the
    # spectra the response is multiplied with are all exactly zero, and so is
    # what comes out: this skips the multiplying then. A new response is still
    # taken in, so the next sound is heard through it. Nothing heard changes.
    ('src/core/overlap_save_convolution_effect.h',
     '    int mNumTailBlocksRemaining;\n'
     '    unique_ptr<OverlapSaveFIR> mPrevFFTIR;\n',
     '    int mNumTailBlocksRemaining;\n'
     '    int mQuietFrames; // q2rtx: frames of silent input in a row, see setup/build_phonon.py\n'
     '    unique_ptr<OverlapSaveFIR> mPrevFFTIR;\n', 1),

    ('src/core/overlap_save_convolution_effect.cpp',
     '    mDryBlockIndex = 0;\n'
     '    mNumTailBlocksRemaining = 0;\n',
     '    mDryBlockIndex = 0;\n'
     '    mQuietFrames = 0;\n'
     '    mNumTailBlocksRemaining = 0;\n', 1),

    ('src/core/overlap_save_convolution_effect.cpp',
     '    auto crossfade = params.fftIR->updateReadBuffer();\n'
     '    if (crossfade)\n',
     '    auto crossfade = params.fftIR->updateReadBuffer();\n'
     '\n'
     '    // q2rtx: the last numBlocks spectra are zero once the input has been\n'
     '    // silent numBlocks + 1 frames (each spans two) - see setup/build_phonon.py\n'
     '    auto quiet = true;\n'
     '    for (auto i = 0; i < mFrameSize && quiet; ++i)\n'
     '        quiet = in[0][i] == 0.0f;\n'
     '    mQuietFrames = quiet ? std::min(mQuietFrames + 1, numBlocks + 1) : 0;\n'
     '    if (mQuietFrames > numBlocks)\n'
     '    {\n'
     '        mFFTWet.zero();\n'
     '        if (crossfade)\n'
     '            mPrevFFTIR.swap(params.fftIR->readBuffer);\n'
     '        mNumTailBlocksRemaining = 0;\n'
     '        return false;\n'
     '    }\n'
     '\n'
     '    if (crossfade)\n', 1),
]

# Exported by the patches above, looked up by steamaudio.c without SA_IMP:
# a phonon.dll that lacks them still loads, without what they bring.
EXTRA_EXPORTS = ['iplq2SetReflectionTracer']


def log(msg):
    print('build_phonon: ' + msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(work, name):
    """Download and unpack one pinned archive; returns its source directory."""
    repo, commit, digest = SOURCES[name]
    src = os.path.join(work, 'src', name)
    stamp = os.path.join(src, '.unpacked-' + commit)
    if os.path.isfile(stamp):
        return src

    archive = os.path.join(work, 'dl', '%s-%s.tar.gz' % (name, commit))
    os.makedirs(os.path.dirname(archive), exist_ok=True)
    if not os.path.isfile(archive) or sha256(archive) != digest:
        url = 'https://github.com/%s/archive/%s.tar.gz' % (repo, commit)
        log('downloading ' + url)
        tmp = archive + '.part'
        with urllib.request.urlopen(url) as r, open(tmp, 'wb') as f:
            shutil.copyfileobj(r, f)
        got = sha256(tmp)
        if got != digest:
            os.remove(tmp)
            sys.exit('build_phonon: %s archive has SHA-256 %s, expected %s' % (name, got, digest))
        os.replace(tmp, archive)

    log('unpacking ' + name)
    if os.path.isdir(src):
        shutil.rmtree(src)
    unpack = src + '.tmp'
    if os.path.isdir(unpack):
        shutil.rmtree(unpack)
    with tarfile.open(archive) as t:
        if hasattr(tarfile, 'data_filter'):
            t.extractall(unpack, filter='data')
        else:
            t.extractall(unpack)
    tops = os.listdir(unpack)
    if len(tops) != 1:
        sys.exit('build_phonon: %s archive does not have one top-level directory' % name)
    os.replace(os.path.join(unpack, tops[0]), src)
    os.rmdir(unpack)
    open(stamp, 'w').close()
    return src


def patch_source(core):
    """Apply PATCHES; one already applied is left as it is."""
    for rel, old, new, count in PATCHES:
        path = os.path.join(core, rel)
        text = open(path, encoding='utf-8', newline='').read()
        if text.count(old) == count:
            open(path, 'w', encoding='utf-8', newline='').write(text.replace(old, new))
            log('patched ' + rel)
        elif text.count(old) or text.count(new) != count:
            sys.exit('build_phonon: %s does not look as PATCHES expects; check the patch '
                     'against this Steam Audio version' % rel)


def generator_args():
    # Ninja when it and the compiler are both on PATH (a developer prompt);
    # otherwise CMake's default, which is Visual Studio on a Windows runner.
    if shutil.which('ninja') and shutil.which('cl'):
        return ['-G', 'Ninja']
    return ['-A', 'x64']


def cmake(*args):
    cmd = ['cmake'] + list(args)
    log(' '.join(cmd))
    subprocess.run(cmd, check=True)


def build_project(work, name, src, flags, target=None):
    """Configure, build and install one CMake project; returns the install prefix."""
    build = os.path.join(work, 'build', name)
    prefix = os.path.join(work, 'install', name)
    stamp = os.path.join(prefix, '.built-' + SOURCES[name][1])
    if os.path.isfile(stamp):
        return prefix

    # The prefix goes in at configure time as well: zlib's old CMake bakes
    # its install directories in then and ignores --install --prefix.
    cmake('-S', src, '-B', build, *generator_args(), *COMMON,
          '-DCMAKE_INSTALL_PREFIX=' + prefix, *flags)
    cmd = ['--build', build, '--config', 'Release', '--parallel']
    if target:
        cmd += ['--target', target]
    cmake(*cmd)
    if os.path.isdir(prefix):
        shutil.rmtree(prefix)
    cmake('--install', build, '--config', 'Release', '--prefix', prefix)
    open(stamp, 'w').close()
    return prefix


def one(pattern):
    hits = glob.glob(pattern, recursive=True)
    if len(hits) != 1:
        sys.exit('build_phonon: expected one match for %s, found %s' % (pattern, hits or 'none'))
    return hits[0]


def copy_tree(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def copy_file(src, dst_dir, name=None):
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src, os.path.join(dst_dir, name or os.path.basename(src)))


def build_deps(work, deps):
    """Build the four dependencies and stage them under core/deps."""

    # zlib: only the static library is wanted, but zlib's install rule names
    # the shared one too, so both are built.
    src = fetch(work, 'zlib')
    zlib = build_project(work, 'zlib', src, ['-DSKIP_INSTALL_FILES=ON'])
    zlib_lib = one(os.path.join(zlib, 'lib', 'zlibstatic.lib'))
    copy_tree(os.path.join(zlib, 'include'), os.path.join(deps, 'zlib', 'include'))
    copy_file(zlib_lib, os.path.join(deps, 'zlib', 'lib', BIN_SUBDIR, 'release'))
    zlib_flags = ['-DZLIB_INCLUDE_DIR=' + os.path.join(deps, 'zlib', 'include'),
                  '-DZLIB_LIBRARY=' + os.path.join(deps, 'zlib', 'lib', BIN_SUBDIR, 'release',
                                                   'zlibstatic.lib')]

    src = fetch(work, 'mysofa')
    mysofa = build_project(work, 'mysofa', src,
                           ['-DBUILD_SHARED_LIBS=FALSE', '-DBUILD_TESTS=FALSE'] + zlib_flags)
    copy_tree(os.path.join(mysofa, 'include'), os.path.join(deps, 'mysofa', 'include'))
    copy_file(one(os.path.join(mysofa, 'lib', '*mysofa*.lib')),
              os.path.join(deps, 'mysofa', 'lib', BIN_SUBDIR, 'release'), 'mysofa.lib')

    # Same flags as Valve's dependencies.json for windows-x64.
    src = fetch(work, 'pffft')
    pffft = build_project(work, 'pffft', src, [
        '-DPFFFT_USE_TYPE_DOUBLE=OFF', '-DPFFFT_USE_FFTPACK=OFF',
        '-DPFFFT_USE_BENCH_GREEN=OFF', '-DPFFFT_USE_BENCH_KISS=OFF', '-DPFFFT_USE_BENCH_POCKET=OFF',
        '-DTARGET_C_ARCH=AVX', '-DTARGET_CXX_ARCH=AVX',
    ])
    copy_file(one(os.path.join(pffft, 'include', '**', 'pffft.h')),
              os.path.join(deps, 'pffft', 'include'))
    copy_file(one(os.path.join(pffft, 'lib', 'pffft.lib')),
              os.path.join(deps, 'pffft', 'lib', BIN_SUBDIR, 'release'))

    # FlatBuffers: headers, plus flatc to compile Steam Audio's schemas.
    src = fetch(work, 'flatbuffers')
    fb = build_project(work, 'flatbuffers', src, [
        '-DFLATBUFFERS_BUILD_FLATLIB=FALSE', '-DFLATBUFFERS_BUILD_FLATHASH=FALSE',
        '-DFLATBUFFERS_BUILD_TESTS=FALSE',
    ])
    copy_tree(os.path.join(fb, 'include'), os.path.join(deps, 'flatbuffers', 'include'))
    copy_file(one(os.path.join(fb, 'bin', 'flatc.exe')),
              os.path.join(deps, 'flatbuffers', 'bin', BIN_SUBDIR))

    return zlib_flags


def build_phonon(work, core, zlib_flags):
    build = os.path.join(work, 'build', 'phonon')
    cmake('-S', core, '-B', build, *generator_args(), *COMMON, *zlib_flags,
          '-DBUILD_SHARED_LIBS=ON',
          '-DSTEAMAUDIO_STATIC_RUNTIME=ON',
          '-DSTEAMAUDIO_ENABLE_IPP=OFF',
          '-DSTEAMAUDIO_ENABLE_MKL=OFF',
          '-DSTEAMAUDIO_ENABLE_EMBREE=OFF',
          '-DSTEAMAUDIO_ENABLE_RADEONRAYS=OFF',
          '-DSTEAMAUDIO_ENABLE_TRUEAUDIONEXT=OFF',
          '-DSTEAMAUDIO_BUILD_TESTS=OFF',
          '-DSTEAMAUDIO_BUILD_BENCHMARKS=OFF',
          '-DSTEAMAUDIO_BUILD_SAMPLES=OFF',
          '-DSTEAMAUDIO_BUILD_ITESTS=OFF',
          '-DSTEAMAUDIO_BUILD_DOCS=OFF')

    # Belt and braces: the options above are the whole point of this build.
    cache = open(os.path.join(build, 'CMakeCache.txt'), encoding='utf-8', errors='replace').read()
    for opt in ('IPP', 'MKL', 'EMBREE', 'RADEONRAYS', 'TRUEAUDIONEXT'):
        if not re.search(r'^STEAMAUDIO_ENABLE_%s:BOOL=OFF$' % opt, cache, re.M):
            sys.exit('build_phonon: STEAMAUDIO_ENABLE_%s is not OFF in the phonon cache' % opt)

    cmake('--build', build, '--config', 'Release', '--parallel', '--target', 'phonon')
    return build


def imports(repo):
    """The functions steamaudio.c looks up in phonon.dll."""
    text = open(os.path.join(repo, 'src', 'client', 'sound', 'steamaudio.c'), encoding='utf-8').read()
    names = re.findall(r'SA_IMP\(\s*[\w ]+\*?\s*,\s*(ipl\w+)\s*,', text)
    if not names:
        sys.exit('build_phonon: found no SA_IMP lines in steamaudio.c')
    return names


def check_dll(dll, names):
    lib = ctypes.WinDLL(dll)
    missing = [n for n in names if not hasattr(lib, n)]
    if missing:
        sys.exit('build_phonon: %s lacks %s' % (dll, ', '.join(missing)))
    data = open(dll, 'rb').read()
    if b'ippGetLibVersion' in data or b'ippsMalloc' in data:
        sys.exit('build_phonon: %s contains Intel IPP' % dll)


def same_text(a, b):
    def norm(p):
        return open(p, 'rb').read().replace(b'\r\n', b'\n')
    return norm(a) == norm(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--repo', default='.')
    ap.add_argument('--work', help='work directory (default: <repo>/build-phonon)')
    ap.add_argument('--update-headers', action='store_true',
                    help='overwrite extern/steamaudio/include with this version')
    args = ap.parse_args()

    if sys.platform != 'win32':
        sys.exit('build_phonon: only the Windows x64 build is set up')

    repo = os.path.abspath(args.repo)
    work = os.path.abspath(args.work or os.path.join(repo, 'build-phonon'))
    os.makedirs(work, exist_ok=True)

    sa = fetch(work, 'steam-audio')
    core = os.path.join(sa, 'core')
    patch_source(core)
    zlib_flags = build_deps(work, os.path.join(core, 'deps'))
    build = build_phonon(work, core, zlib_flags)

    dll = one(os.path.join(build, '**', 'phonon.dll'))
    check_dll(dll, imports(repo) + EXTRA_EXPORTS)

    # Headers: the source tree has phonon.h, the build generates phonon_version.h.
    inc = os.path.join(repo, 'extern', 'steamaudio', 'include')
    os.makedirs(inc, exist_ok=True)
    fresh = {'phonon.h': os.path.join(core, 'src', 'core', 'phonon.h'),
             'phonon_version.h': one(os.path.join(build, '**', 'phonon_version.h'))}
    for h in HEADERS:
        dst = os.path.join(inc, h)
        if os.path.isfile(dst) and not same_text(dst, fresh[h]) and not args.update_headers:
            sys.exit('build_phonon: %s differs from Steam Audio %s; rerun with --update-headers '
                     'if the version bump is intended' % (dst, STEAMAUDIO_VERSION))
        if not os.path.isfile(dst) or args.update_headers:
            shutil.copy2(fresh[h], dst)

    out = os.path.join(repo, 'phonon.dll')
    shutil.copy2(dll, out)
    log('wrote %s (%.1f MB), Steam Audio %s without IPP'
        % (out, os.path.getsize(out) / 1e6, STEAMAUDIO_VERSION))
    return 0


if __name__ == '__main__':
    sys.exit(main())
