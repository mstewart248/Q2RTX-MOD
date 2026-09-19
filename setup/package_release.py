#!/usr/bin/env python3
"""
Assemble a complete, playable Quake II RTX Overdrive package.

    python setup/package_release.py --out Q2RTX-Overdrive-win64.zip [--dlss-dir DIR]

Run from the repository root. Used by .github/workflows/release.yml and usable
by hand for a local package; both go through here so a release built on CI and
one built on a workstation contain the same thing.

WHAT GOES IN, and the rule behind it:

  * every file git TRACKS under baseq2/, rerelease/ and rogue/. That is the
    authored content, and using git as the manifest is what keeps a working tree
    full of local experiments - savegames, configs, map compiles, scratch cfgs -
    out of a release without needing a hand-maintained exclude list.
  * the build outputs: q2rtx.exe, q2rtxded.exe, q2rtxded-x86.exe if it was
    built, baseq2/gamex86_64.dll, and the compiled shaders both loose in
    baseq2/shader_vkpt and packed in baseq2/shaders.pkz.
  * the stock Quake II RTX media that is too large for git: q2rtx_media.pkz and
    blue_noise.pkz. CI restores these from the `shipped-media` release.
  * the three DLSS runtime redistributables, taken from the SDK's
    lib/Windows_x86_64/rel directory.
  * the repo documentation plus a generated README-FIRST.txt.

WHAT STAYS OUT:

  * every .pak file. They are id Software's and cannot be redistributed; the
    README tells the player where to get their own.
  * music/ and video/ - the player's own copies from the remaster.
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

REQUIRED_ROOT = ['q2rtx.exe', 'q2rtxded.exe']
OPTIONAL_ROOT = ['q2rtxded-x86.exe']
DOCS = ['license.txt', 'notice.txt', 'readme.md', 'changelog.md']

# Ray Reconstruction is served by nvngx_dlssd.dll. nvngx_dlssnr.dll is
# deliberately not shipped: it is a separate 165 MB snippet that is not part of
# the SDK's redistributable set and fails signature validation at load.
DLSS_DLLS = ['nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll']

REQUIRED_BASEQ2 = [
    'baseq2/gamex86_64.dll',
    'baseq2/q2rtx_media.pkz',
    'baseq2/blue_noise.pkz',
    'baseq2/shaders.pkz',
]


def tracked(repo, prefix):
    out = subprocess.run(['git', '-C', repo, 'ls-files', prefix],
                         capture_output=True, text=True, check=True).stdout
    return [l.strip() for l in out.splitlines() if l.strip()]


def build_manifest(repo, dlss_dir):
    members, seen, missing, skipped = [], set(), [], []

    def add(disk, arc, required):
        if arc in seen:
            return
        if not os.path.isfile(disk):
            (missing if required else skipped).append(arc)
            return
        seen.add(arc)
        members.append((disk, arc))

    for f in REQUIRED_ROOT + DOCS + REQUIRED_BASEQ2:
        add(os.path.join(repo, f), f, True)
    for f in OPTIONAL_ROOT:
        add(os.path.join(repo, f), f, False)

    for f in DLSS_DLLS:
        # Prefer the SDK checkout; fall back to a copy sitting next to the exe.
        cand = os.path.join(dlss_dir, f) if dlss_dir else None
        if cand and os.path.isfile(cand):
            add(cand, f, True)
        else:
            add(os.path.join(repo, f), f, True)

    shaders = os.path.join(repo, 'baseq2', 'shader_vkpt')
    if not os.path.isdir(shaders):
        missing.append('baseq2/shader_vkpt/')
    else:
        for n in sorted(os.listdir(shaders)):
            add(os.path.join(shaders, n), 'baseq2/shader_vkpt/' + n, True)

    for prefix in ('baseq2', 'rerelease', 'rogue'):
        for f in tracked(repo, prefix):
            add(os.path.join(repo, f), f, False)

    return members, missing, skipped


def readme_text():
    return open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             'README-FIRST.txt'), 'rb').read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--repo', default='.')
    ap.add_argument('--dlss-dir', default='',
                    help='DLSS SDK lib/Windows_x86_64/rel directory')
    args = ap.parse_args()

    repo = os.path.abspath(args.repo)
    members, missing, skipped = build_manifest(repo, args.dlss_dir)

    if missing:
        print('ABORT - required files absent:', file=sys.stderr)
        for m in missing:
            print('   ' + m, file=sys.stderr)
        return 1

    if skipped:
        print('%d tracked path(s) absent from the working tree, skipped' % len(skipped))

    raw = sum(os.path.getsize(d) for d, _ in members)
    print('%d files, %.2f GB raw -> %s' % (len(members), raw / 1e9, args.out))

    if os.path.exists(args.out):
        os.remove(args.out)

    with zipfile.ZipFile(args.out, 'w', zipfile.ZIP_DEFLATED,
                         allowZip64=True, compresslevel=6) as z:
        z.writestr(TOP + '/README-FIRST.txt', readme_text())
        for disk, arc in members:
            method = (zipfile.ZIP_STORED if arc.lower().endswith(STORE_EXT)
                      else zipfile.ZIP_DEFLATED)
            z.write(disk, TOP + '/' + arc, compress_type=method)

    size = os.path.getsize(args.out)
    print('wrote %.2f GB (%.1f%% of raw)' % (size / 1e9, 100.0 * size / raw))

    # A GitHub release asset may not exceed 2 GiB.
    limit = 2 * 1024 ** 3
    if size >= limit:
        print('ERROR: %.2f GB exceeds the 2 GiB per-asset limit' % (size / 1e9),
              file=sys.stderr)
        return 1
    print('headroom under the 2 GiB asset limit: %.2f GB' % ((limit - size) / 1e9))
    return 0


if __name__ == '__main__':
    sys.exit(main())
