#!/usr/bin/env python3
"""
Stage the DirectX 12 Agility SDK for the D3D12 renderer (vid_rtx 3,
src/refresh/d3d12).

    python setup/fetch_agility_sdk.py

Run from the repository root. Needs nothing but Python 3: the SDK is a NuGet
package (a zip), downloaded from nuget.org and checked against the SHA-256
pinned below.

WHY TWO VERSIONS:

  * RETAIL (1.620.1) - DXR 1.2 (Shader Execution Reordering, Opacity
    Micromaps), Shader Model 6.9. This is what the renderer loads by default.
  * PREVIEW (1.722.0-preview) - adds D3D12_RAYTRACING_TIER_2_0: Clustered
    Geometry (CLAS / clustered BLAS) and Partitioned TLAS, the standardised
    form of NVIDIA's RTX Mega Geometry; plus linear algebra (cooperative
    vectors). A preview D3D12Core.dll only loads with Windows Developer Mode
    on; d3d12_agility_preview 1 asks for it and falls back to retail when it
    will not load.

The headers come from the PREVIEW package, which is a superset of retail
(D3D12_SDK_VERSION is 620 in both; the preview adds D3D12_PREVIEW_SDK_VERSION
722). Compiling against them and running the retail runtime is fine: every
preview-only call is behind a CheckFeatureSupport answer.

The exe exports no D3D12SDKVersion / D3D12SDKPath. d3d12/device.c picks the
runtime with ID3D12SDKConfiguration1::CreateDeviceFactory instead, so nothing
else in the process that loads d3d12.dll (NGX, Reflex, overlays) is moved off
the system runtime by having a D3D12 renderer built in.

OUTPUTS (all gitignored):

  * extern/agility-sdk/include      d3d12.h, d3d12sdklayers.h, d3dx12/, ...
  * D3D12/D3D12Core.dll             retail runtime, next to q2rtx.exe
  * D3D12/d3d12SDKLayers.dll        its debug layer (d3d12_debug 1)
  * D3D12-preview/...               the same from the preview package

Downloads are cached in build-agility/ (gitignored).
"""

import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile

PACKAGES = {
    # name: (version, sha256 of the .nupkg)
    'retail':  ('1.620.1',         'a8a494c3e5f687d9be0a465ec8248da70781ab689d888db8806b600422722be6'),
    'preview': ('1.722.0-preview', 'bf9915dbb39f7c97018d42b36e65469f11c864b805e3bc9a773eba364208cdc3'),
}

# Where each package's runtime goes, relative to the repository root.
RUNTIME_DIRS = {'retail': 'D3D12', 'preview': 'D3D12-preview'}
RUNTIME_FILES = ('D3D12Core.dll', 'd3d12SDKLayers.dll')
HEADERS_FROM = 'preview'

URL = 'https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/{v}/microsoft.direct3d.d3d12.{v}.nupkg'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(root, name):
    version, want = PACKAGES[name]
    cache = os.path.join(root, 'build-agility')
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, 'microsoft.direct3d.d3d12.%s.nupkg' % version)
    if not os.path.exists(path) or sha256(path) != want:
        print('downloading Agility SDK %s' % version)
        tmp = path + '.part'
        urllib.request.urlretrieve(URL.format(v=version), tmp)
        got = sha256(tmp)
        if got != want:
            os.remove(tmp)
            sys.exit('%s: SHA-256 %s, expected %s - refusing to use it' % (version, got, want))
        os.replace(tmp, path)
    return zipfile.ZipFile(path)


def extract_prefix(z, prefix, dest):
    n = 0
    for info in z.infolist():
        if info.is_dir() or not info.filename.startswith(prefix):
            continue
        rel = info.filename[len(prefix):]
        # never let an archive name climb out of dest
        if rel.startswith('/') or '..' in rel.split('/'):
            sys.exit('bad path in package: %s' % info.filename)
        out = os.path.join(dest, *rel.split('/'))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with z.open(info) as src, open(out, 'wb') as dst:
            shutil.copyfileobj(src, dst)
        n += 1
    return n


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for name in PACKAGES:
        z = fetch(root, name)
        rt = os.path.join(root, RUNTIME_DIRS[name])
        os.makedirs(rt, exist_ok=True)
        for f in RUNTIME_FILES:
            with z.open('build/native/bin/x64/' + f) as src, open(os.path.join(rt, f), 'wb') as dst:
                shutil.copyfileobj(src, dst)
        print('%s %s -> %s' % (name, PACKAGES[name][0], RUNTIME_DIRS[name]))
        if name == HEADERS_FROM:
            inc = os.path.join(root, 'extern', 'agility-sdk', 'include')
            if os.path.isdir(inc):
                shutil.rmtree(inc)
            n = extract_prefix(z, 'build/native/include/', inc)
            with open(os.path.join(root, 'extern', 'agility-sdk', 'VERSION.txt'), 'w') as f:
                f.write('headers: Microsoft.Direct3D.D3D12 %s\n' % PACKAGES[name][0])
                for k, (v, h) in PACKAGES.items():
                    f.write('%s runtime: %s (%s) sha256 %s\n' % (k, v, RUNTIME_DIRS[k], h))
            print('headers (%d files) -> extern/agility-sdk/include' % n)


if __name__ == '__main__':
    main()
