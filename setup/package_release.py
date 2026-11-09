#!/usr/bin/env python3
"""
Assemble a complete, playable Quake II RTX Overdrive package.

    python setup/package_release.py --out Q2RTX-Overdrive-win64.zip
    python setup/package_release.py --out Q2RTX-Overdrive-win64.zip \
                                    --media-out Q2RTX-Overdrive-media.zip

Run from the repository root. Used by .github/workflows/release.yml and usable
by hand for a local package; both go through here so a release built on CI and
one built on a workstation contain the same thing.

WHY A RELEASE IS TWO ZIPS. A GitHub release asset may not exceed 2 GiB
(2.15 GB), and from v1.0.8 on - when the material work added ~370 MB of
textures - the single package came to 2.36 GB and the release job failed. The
stock media (MEDIA below, ~1.0 GB) is the natural part to split off: it is
already compressed, so it is stored at full size; it never changes, so a player
updating can keep the copy they have; and nothing about it depends on the
build. --media-out writes it to a zip of its own under the same top folder, so
extracting both zips into one place gives the complete install. Without
--media-out everything goes in --out, as before. Every zip written is checked
against the limit.

WHAT GOES IN, and the rule behind it:

  * every file git TRACKS under baseq2/, rerelease/ and rogue/. That is the
    authored content, and using git as the manifest is what keeps a working tree
    full of local experiments - savegames, configs, map compiles, scratch cfgs -
    out of a release without needing a hand-maintained exclude list.
  * the build outputs: q2rtx.exe, q2rtxded.exe, q2rtxded-x86.exe if it was
    built, baseq2/gamex86_64.dll, the compiled shaders both loose in
    baseq2/shader_vkpt and packed in baseq2/shaders.pkz, and the Direct3D 12
    renderer's DXIL loose in baseq2/shader_d3d12.
  * phonon.dll, the Steam Audio runtime, as built from source by
    setup/build_phonon.py, with its Apache-2.0 licence and third-party notices
    from extern/steamaudio/ under licenses/steam-audio/.
  * nvflow.dll and nvflowext.dll, the NVIDIA Flow 2 runtime behind the
    simulated fire and smoke, as built from source by setup/build_flow.py,
    with its BSD-3-Clause licence from extern/flow/ under licenses/nvidia-flow/.
  * the stock Quake II RTX media that is too large for git: q2rtx_media.pkz and
    blue_noise.pkz. CI restores these from the `shipped-media` release. With
    --media-out they go in the second zip.
  * the repo documentation plus a generated README-FIRST.txt.

WHAT STAYS OUT:

  * every NVIDIA DLSS / NGX runtime DLL (nvngx_*.dll). They are NOT part of
    a release: players supply their own copies and put them beside q2rtx.exe.
    build_manifest refuses to package one even if it turns up somewhere the
    rules above would otherwise pick it up.
  * Microsoft's DirectX 12 Agility SDK runtime (D3D12Core.dll,
    d3d12SDKLayers.dll, d3dconfig.exe), which setup/fetch_agility_sdk.py
    stages in D3D12/ for a source build. Microsoft allows redistributing it, but
    it is closed source, so it stays out of a GPL package by the same rule as the
    DLSS DLLs. The D3D12 renderer runs on Windows 11's own runtime; README-FIRST
    tells Windows 10 players where to get the DLL.
  * Valve's PREBUILT phonon.dll from the Steam Audio SDK zip. It links Intel
    IPP and oneMKL, which are not open source and cannot ship with a GPL
    program; the package is refused if the phonon.dll found carries them.
  * every .pak file. They are id Software's and cannot be redistributed; the
    README tells the player where to get their own.
  * music/ and video/ - the player's own copies from the remaster.
  * rerelease/screenshots/, although tracked: 111 MB of the readme's images.
    The game never reads them, and they would land in the player's own
    screenshot folder.
  * anything untracked that is not named above. Notably shader_balls.pkz (a
    202 MB Q2RTX test scene nothing loads during play) and
    blue_noise_restir.pkz (a duplicate of blue_noise.pkz).

Members that are already compressed are STORED rather than deflated;
recompressing a zip inside a zip only costs time. Everything else is deflated,
which matters because the bulk of the content is uncompressed TGA.
"""

import argparse
import os
import subprocess
import sys
import zipfile

TOP = 'Q2RTX-Overdrive'

# Already-compressed payloads - storing them is both faster and no larger.
STORE_EXT = ('.pkz', '.png', '.jpg', '.jpeg', '.kpf', '.mp3', '.ogg', '.zip')

REQUIRED_ROOT = ['q2rtx.exe', 'q2rtxded.exe', 'phonon.dll', 'nvflow.dll', 'nvflowext.dll']
OPTIONAL_ROOT = ['q2rtxded-x86.exe']
DOCS = ['license.txt', 'notice.txt', 'readme.md', 'changelog.md']

# NVIDIA's DLSS runtime DLLs and Microsoft's Agility SDK runtime never go in a
# release - players bring their own. Anything matching this is refused wherever
# it comes from. (DLSS 5 used to need our own nvngx.dll_dlssnr.dll forwarder
# here; it goes through the NGX core now.)
AGILITY_RUNTIME = ('d3d12core.dll', 'd3d12sdklayers.dll', 'd3dconfig.exe')

def is_vendor_runtime(arc):
    n = os.path.basename(arc).lower()
    return (n.startswith('nvngx') and n.endswith('.dll')) or n in AGILITY_RUNTIME

# Steam Audio's licence and the notices for what is compiled into phonon.dll,
# and NVIDIA Flow's licence: (file in the repository, path in the package).
LICENSES = [
    ('extern/steamaudio/LICENSE.md', 'licenses/steam-audio/LICENSE.md'),
    ('extern/steamaudio/THIRDPARTY.md', 'licenses/steam-audio/THIRDPARTY.md'),
    ('extern/flow/LICENSE.md', 'licenses/nvidia-flow/LICENSE.md'),
]

# Valve's prebuilt phonon.dll links Intel IPP (its PE sections are named
# IPPCODE/IPPDATA) and oneMKL; setup/build_phonon.py builds without either.
PROPRIETARY_MARKERS = (b'IPPCODE', b'IPPDATA', b'Intel oneMKL')

def has_proprietary_code(path):
    data = open(path, 'rb').read()
    return [m.decode() for m in PROPRIETARY_MARKERS if m in data]

REQUIRED_BASEQ2 = [
    'baseq2/gamex86_64.dll',
    'baseq2/shaders.pkz',
]

# The stock media: required, and what --media-out splits into a zip of its own.
MEDIA = [
    'baseq2/q2rtx_media.pkz',
    'baseq2/blue_noise.pkz',
]

# Tracked but not for players (see WHAT STAYS OUT).
EXCLUDE_TRACKED = ('rerelease/screenshots/',)

# A GitHub release asset may not exceed 2 GiB.
ASSET_LIMIT = 2 * 1024 ** 3


def tracked(repo, prefix):
    out = subprocess.run(['git', '-C', repo, 'ls-files', prefix],
                         capture_output=True, text=True, check=True).stdout
    return [l.strip() for l in out.splitlines() if l.strip()]


def build_manifest(repo):
    members, seen, missing, skipped, refused = [], set(), [], [], []

    def add(disk, arc, required):
        if arc in seen:
            return
        if is_vendor_runtime(arc):
            refused.append(arc)
            return
        if not os.path.isfile(disk):
            (missing if required else skipped).append(arc)
            return
        seen.add(arc)
        members.append((disk, arc))

    for f in REQUIRED_ROOT + DOCS + REQUIRED_BASEQ2 + MEDIA:
        add(os.path.join(repo, f), f, True)
    for f in OPTIONAL_ROOT:
        add(os.path.join(repo, f), f, False)
    for disk, arc in LICENSES:
        add(os.path.join(repo, disk), arc, True)

    phonon = os.path.join(repo, 'phonon.dll')
    if os.path.isfile(phonon):
        found = has_proprietary_code(phonon)
        if found:
            refused.append('phonon.dll (prebuilt with Intel IPP/oneMKL: %s) - '
                           'run setup/build_phonon.py' % ', '.join(found))

    shaders = os.path.join(repo, 'baseq2', 'shader_vkpt')
    if not os.path.isdir(shaders):
        missing.append('baseq2/shader_vkpt/')
    else:
        for n in sorted(os.listdir(shaders)):
            add(os.path.join(shaders, n), 'baseq2/shader_vkpt/' + n, True)

    # The D3D12 renderer (vid_rtx 3) reads shader_d3d12/<name>.dxil through the
    # filesystem, loose; there is no pkz for it, so nothing stale can shadow it.
    shaders = os.path.join(repo, 'baseq2', 'shader_d3d12')
    if not os.path.isdir(shaders):
        missing.append('baseq2/shader_d3d12/ (build with CONFIG_D3D12_RENDERER: '
                       'run setup/fetch_agility_sdk.py, and have a Vulkan SDK)')
    else:
        for n in sorted(os.listdir(shaders)):
            if n.endswith('.dxil'):
                add(os.path.join(shaders, n), 'baseq2/shader_d3d12/' + n, True)

    for prefix in ('baseq2', 'rerelease', 'rogue'):
        for f in tracked(repo, prefix):
            if not f.startswith(EXCLUDE_TRACKED):
                add(os.path.join(repo, f), f, False)

    return members, missing, skipped, refused


def readme_text():
    return open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             'README-FIRST.txt'), 'rb').read()


def write_package(out, members, with_readme):
    """Write one zip; False if it is over the release asset limit."""
    raw = sum(os.path.getsize(d) for d, _ in members)
    print('%d files, %.2f GB raw -> %s' % (len(members), raw / 1e9, out))

    if os.path.exists(out):
        os.remove(out)

    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED,
                         allowZip64=True, compresslevel=6) as z:
        if with_readme:
            z.writestr(TOP + '/README-FIRST.txt', readme_text())
        for disk, arc in members:
            method = (zipfile.ZIP_STORED if arc.lower().endswith(STORE_EXT)
                      else zipfile.ZIP_DEFLATED)
            z.write(disk, TOP + '/' + arc, compress_type=method)

    size = os.path.getsize(out)
    print('wrote %.2f GB (%.1f%% of raw)' % (size / 1e9, 100.0 * size / raw))

    if size >= ASSET_LIMIT:
        print('ERROR: %s is %.2f GB, over the 2 GiB (%.2f GB) per-asset limit'
              % (out, size / 1e9, ASSET_LIMIT / 1e9), file=sys.stderr)
        return False
    print('headroom under the 2 GiB asset limit: %.2f GB'
          % ((ASSET_LIMIT - size) / 1e9))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--media-out',
                    help='write the stock media (%s) to this second zip '
                         'instead of --out' % ', '.join(MEDIA))
    ap.add_argument('--repo', default='.')
    args = ap.parse_args()

    repo = os.path.abspath(args.repo)
    members, missing, skipped, refused = build_manifest(repo)

    bad_phonon = [r for r in refused if r.startswith('phonon.dll')]
    refused = [r for r in refused if not r.startswith('phonon.dll')]
    if refused:
        print('left out NVIDIA / Agility SDK runtime file(s) - players supply their own:')
        for r in refused:
            print('   ' + r)

    if bad_phonon:
        print('ABORT - ' + bad_phonon[0], file=sys.stderr)
        return 1

    if missing:
        print('ABORT - required files absent:', file=sys.stderr)
        for m in missing:
            print('   ' + m, file=sys.stderr)
        return 1

    if skipped:
        print('%d tracked path(s) absent from the working tree, skipped' % len(skipped))

    if args.media_out:
        media = [m for m in members if m[1] in MEDIA]
        main_members = [m for m in members if m[1] not in MEDIA]
        packages = [(args.out, main_members, True), (args.media_out, media, False)]
    else:
        packages = [(args.out, members, True)]

    # Write every zip before failing, so one run reports all their sizes.
    ok = [write_package(out, mem, readme) for out, mem, readme in packages]
    if not all(ok):
        if not args.media_out:
            print('split the stock media off with --media-out', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
