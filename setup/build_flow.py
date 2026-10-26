#!/usr/bin/env python3
"""
Build nvflow.dll and nvflowext.dll - the NVIDIA Flow 2 runtime (GPU smoke and
fire simulation) that src/refresh/vkpt/flow.c loads - from source.

    python setup/build_flow.py

Run from the repository root, in an x64 Visual Studio developer prompt with
CMake, Ninja and git on PATH. Without the two DLLs the game runs as before and
the Flow options stay hidden.

WHAT FLOW IS: a sparse-grid gaseous fluid solver (advection, combustion,
vorticity, pressure) that runs as Vulkan compute. It lives in the flow/
directory of NVIDIA's PhysX repository, BSD-3 licensed; the licence is copied
to extern/flow/LICENSE.md and ships next to the DLLs.

WHAT IT DOES:

  * checks out flow/ of the pinned PhysX commit (a sparse git clone, ~10 MB
    instead of the whole repository);
  * downloads the Slang release Flow's dependencies.xml names and checks its
    SHA-256 - Flow's shaders are HLSL compiled to SPIR-V by Slang at build
    time and embedded in the DLLs;
  * adds setup/flow/ExternalVulkan.cpp and NvFlowVulkanExternal.h to the
    source and applies PATCHES (see there), so Flow can run on the game's own
    VkDevice and queue rather than creating a second device;
  * builds with setup/flow/CMakeLists.txt, which replaces Flow's premake and
    packman bootstrap (packman fetches its own Python and toolchain from
    NVIDIA's CDN; nothing here needs it).

OUTPUTS:

  * nvflow.dll and nvflowext.dll at the repository root, next to q2rtx.exe
    (gitignored as *.dll).
  * extern/flow/include: Flow's public headers plus NvFlowVulkanExternal.h,
    written when absent. When present they must match this source, or the
    script stops - compiling against one version and loading another is how a
    struct layout silently goes wrong. Pass --update-headers after a bump.

Work files go to build-flow/ (gitignored); a second run reuses the checkout,
the Slang download and the CMake build.
"""

import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

# NVIDIA-Omniverse/PhysX at the commit whose flow/ is Flow 2.2.0 (CHANGELOG.md).
PHYSX_REPO = 'https://github.com/NVIDIA-Omniverse/PhysX.git'
PHYSX_COMMIT = 'b97365c6ddb32fda8469bc788ec18024862b0368'

# The Slang version flow/dependencies.xml asks packman for, from Slang's own
# GitHub release instead: (url, SHA-256).
SLANG = {
    'win32': ('https://github.com/shader-slang/slang/releases/download/v2025.6.1/'
              'slang-2025.6.1-windows-x86_64.zip',
              '47ef4f7f2df17f9965612bc39e63dac0d9748985887339bb5f11f2d931c0cb61'),
}

# Files added to the Flow source: (file in setup/flow, destination under flow/).
ADDITIONS = [
    ('ExternalVulkan.cpp', 'source/nvflowext/vulkan/ExternalVulkan.cpp'),
    ('NvFlowVulkanExternal.h', 'include/nvflowext/NvFlowVulkanExternal.h'),
]

# Changes to the Flow source: (file under flow/, text, replacement, times the
# text occurs). Written with '\n'; matched against CRLF checkouts as well.
PATCHES = [
    # Windows builds compile every shader twice, for Vulkan and through DXC
    # for D3D12, and the D3D12 pass needs a DXC Flow only gets from packman.
    # Flow 2 has no D3D12 backend in this source and the game is Vulkan, so
    # compile the Vulkan pass only, as Linux builds already do.
    ('source/nvflowshadercompiler/ShaderPreprocessor.cpp',
     '#if defined(_WIN32)\n'
     '    NvFlowUint64 slangPasses = 2u;\n',
     '#if defined(_WIN32)\n'
     '    NvFlowUint64 slangPasses = 1u; // q2rtx: Vulkan only, see setup/build_flow.py\n', 1),

    # ExternalVulkan.cpp builds the device manager and device around the
    # game's instance and device; these flags keep Flow from destroying them.
    ('source/nvflowext/vulkan/DeviceVulkan.h',
     '    struct DeviceManager\n'
     '    {\n'
     '        void* vulkan_module = nullptr;\n',
     '    struct DeviceManager\n'
     '    {\n'
     '        NvFlowBool32 external = NV_FLOW_FALSE; // q2rtx: instance owned by the application\n'
     '        void* vulkan_module = nullptr;\n', 1),
    ('source/nvflowext/vulkan/DeviceVulkan.h',
     '    struct Device\n'
     '    {\n'
     '        NvFlowDeviceDesc desc = {};\n',
     '    struct Device\n'
     '    {\n'
     '        NvFlowBool32 external = NV_FLOW_FALSE; // q2rtx: device owned by the application\n'
     '        NvFlowDeviceDesc desc = {};\n', 1),
    ('source/nvflowext/vulkan/DeviceVulkan.cpp',
     '    ptr->loader.vkDestroyInstance(ptr->vulkanInstance, nullptr);\n'
     '\n'
     '    NvFlowFreeLibrary(ptr->vulkan_module);\n',
     '    if (!ptr->external) // q2rtx\n'
     '    {\n'
     '        ptr->loader.vkDestroyInstance(ptr->vulkanInstance, nullptr);\n'
     '\n'
     '        NvFlowFreeLibrary(ptr->vulkan_module);\n'
     '    }\n', 1),
    ('source/nvflowext/vulkan/DeviceVulkan.cpp',
     '    deviceQueue_destroy(ptr->deviceQueue);\n'
     '\n'
     '    ptr->loader.vkDestroyDevice(ptr->vulkanDevice, nullptr);\n',
     '    deviceQueue_destroy(ptr->deviceQueue);\n'
     '\n'
     '    if (!ptr->external) // q2rtx\n'
     '    {\n'
     '        ptr->loader.vkDestroyDevice(ptr->vulkanDevice, nullptr);\n'
     '    }\n', 1),

    # EmitterNanoVdbCS - the NanoVDB volume emitter, which the game never
    # uses - is 4.7 MB of SPIR-V: Slang inlines the whole NanoVDB tree walk
    # into it. Every other Flow shader is under 70 KB. The NVIDIA compiler
    # (driver 5xx, RTX 5070 Ti) did not finish it in minutes, and Flow builds
    # every op's pipelines inside createGrid, so the game hung at startup.
    # Compile it the first time a NanoVDB emitter actually runs instead.
    ('source/nvflowext/EmitterNanoVdb.cpp',
     '        NvFlowContextInterface_duplicate(&ptr->contextInterface, in->contextInterface);\n'
     '\n'
     '        EmitterNanoVdbCS_init(&ptr->contextInterface, in->context, &ptr->emitterNanoVdbCS);\n',
     '        NvFlowContextInterface_duplicate(&ptr->contextInterface, in->contextInterface);\n'
     '\n'
     '        // q2rtx: EmitterNanoVdbCS is compiled on first use, see setup/build_flow.py\n', 1),
    ('source/nvflowext/EmitterNanoVdb.cpp',
     '        ptr->contextInterface.destroySampler(in->context, ptr->samplerLinear);\n'
     '\n'
     '        EmitterNanoVdbCS_destroy(in->context, &ptr->emitterNanoVdbCS);\n',
     '        ptr->contextInterface.destroySampler(in->context, ptr->samplerLinear);\n'
     '\n'
     '        if (ptr->emitterNanoVdbCS.pipeline) // q2rtx: compiled on first use\n'
     '        {\n'
     '            EmitterNanoVdbCS_destroy(in->context, &ptr->emitterNanoVdbCS);\n'
     '        }\n', 1),
    ('source/nvflowext/EmitterNanoVdb.cpp',
     '            gridDim.z = 1u;\n'
     '\n'
     '            EmitterNanoVdbCS_addPassCompute(in->context, &ptr->emitterNanoVdbCS, gridDim, &passParams);\n',
     '            gridDim.z = 1u;\n'
     '\n'
     '            if (!ptr->emitterNanoVdbCS.pipeline) // q2rtx: see setup/build_flow.py\n'
     '            {\n'
     '                EmitterNanoVdbCS_init(&ptr->contextInterface, in->context, &ptr->emitterNanoVdbCS);\n'
     '            }\n'
     '            EmitterNanoVdbCS_addPassCompute(in->context, &ptr->emitterNanoVdbCS, gridDim, &passParams);\n', 1),
]

# The functions flow.c looks up in each DLL.
EXPORTS = {
    'nvflow': ['NvFlowGetOpList'],
    'nvflowext': ['NvFlowGetExtOpList', 'NvFlowGetGridInterface', 'NvFlowGetGridParamsInterface',
                  'NvFlowGetDeviceInterface', 'NvFlowVulkanExternalCreateDeviceManager',
                  'NvFlowVulkanExternalCreateDevice', 'NvFlowVulkanExternalGetTexture',
                  'NvFlowVulkanExternalGetBuffer'],
}

HEADERS = [
    'nvflow/NvFlow.h',
    'nvflow/NvFlowContext.h',
    'nvflow/NvFlowReflect.h',
    'nvflow/NvFlowTypes.h',
    'nvflow/shaders/NvFlowShaderTypes.h',
    'nvflowext/NvFlowExt.h',
    'nvflowext/NvFlowVulkanExternal.h',
]


def log(msg):
    print('build_flow: ' + msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def run(cmd, cwd=None):
    log(' '.join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def git_head(repo):
    r = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=repo, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def fetch_flow(work):
    """Sparse checkout of flow/ at PHYSX_COMMIT; returns the flow directory."""
    repo = os.path.join(work, 'physx')
    flow = os.path.join(repo, 'flow')
    if os.path.isdir(os.path.join(repo, '.git')) and git_head(repo) == PHYSX_COMMIT:
        return flow
    if os.path.isdir(repo):
        shutil.rmtree(repo)
    run(['git', 'clone', '--filter=blob:none', '--sparse', '--no-checkout', PHYSX_REPO, repo])
    run(['git', 'sparse-checkout', 'set', 'flow'], cwd=repo)
    run(['git', 'checkout', '--quiet', PHYSX_COMMIT], cwd=repo)
    if git_head(repo) != PHYSX_COMMIT:
        sys.exit('build_flow: checkout did not land on ' + PHYSX_COMMIT)
    return flow


def fetch_slang(work, flow):
    """Download and unpack Slang into flow/external/slang, where Flow looks."""
    if sys.platform not in SLANG:
        sys.exit('build_flow: no Slang release is pinned for ' + sys.platform)
    url, digest = SLANG[sys.platform]
    dst = os.path.join(flow, 'external', 'slang')
    stamp = os.path.join(dst, '.unpacked-' + digest[:16])
    if os.path.isfile(stamp):
        return

    archive = os.path.join(work, 'dl', url.rsplit('/', 1)[1])
    os.makedirs(os.path.dirname(archive), exist_ok=True)
    if not os.path.isfile(archive) or sha256(archive) != digest:
        log('downloading ' + url)
        tmp = archive + '.part'
        with urllib.request.urlopen(url) as r, open(tmp, 'wb') as f:
            shutil.copyfileobj(r, f)
        got = sha256(tmp)
        if got != digest:
            os.remove(tmp)
            sys.exit('build_flow: Slang archive has SHA-256 %s, expected %s' % (got, digest))
        os.replace(tmp, archive)

    log('unpacking Slang')
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            target = os.path.realpath(os.path.join(dst, name))
            if not target.startswith(os.path.realpath(dst) + os.sep):
                sys.exit('build_flow: Slang archive entry escapes the target: ' + name)
        z.extractall(dst)
    open(stamp, 'w').close()


def patch_source(flow, setup):
    """Copy ADDITIONS and apply PATCHES; a patch already applied is left alone."""
    for src, rel in ADDITIONS:
        dst = os.path.join(flow, rel)
        if not os.path.isfile(dst) or not same_text(os.path.join(setup, src), dst):
            shutil.copy2(os.path.join(setup, src), dst)
            log('added ' + rel)

    for rel, old, new, count in PATCHES:
        path = os.path.join(flow, rel)
        text = open(path, encoding='utf-8', newline='').read()
        if '\r\n' in text:
            old, new = old.replace('\n', '\r\n'), new.replace('\n', '\r\n')
        if text.count(old) == count:
            open(path, 'w', encoding='utf-8', newline='').write(text.replace(old, new))
            log('patched ' + rel)
        elif text.count(old) or text.count(new) != count:
            sys.exit('build_flow: %s does not look as PATCHES expects; check the patch '
                     'against this Flow version' % rel)


def build(work, flow, setup):
    out = os.path.join(work, 'build')
    # Ninja when it and the compiler are both on PATH (a developer prompt);
    # otherwise CMake's default, which is Visual Studio on a Windows runner.
    # Both put the DLLs in the same place (RUNTIME_OUTPUT_DIRECTORY_RELEASE).
    gen = ['-G', 'Ninja'] if shutil.which('ninja') and shutil.which('cl') else ['-A', 'x64']
    run(['cmake', '-S', setup, '-B', out, *gen, '-DCMAKE_BUILD_TYPE=Release',
         '-DFLOW_ROOT=' + flow.replace('\\', '/')])
    run(['cmake', '--build', out, '--config', 'Release', '--parallel'])
    return os.path.join(flow, '_build', 'windows-x86_64' if sys.platform == 'win32' else 'linux-x86_64', 'release')


def check_dll(dll, names):
    import ctypes
    lib = ctypes.CDLL(dll)
    missing = [n for n in names if not hasattr(lib, n)]
    if missing:
        sys.exit('build_flow: %s lacks %s' % (dll, ', '.join(missing)))


def same_text(a, b):
    def norm(p):
        return open(p, 'rb').read().replace(b'\r\n', b'\n')
    return norm(a) == norm(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--repo', default='.')
    ap.add_argument('--work', help='work directory (default: <repo>/build-flow)')
    ap.add_argument('--update-headers', action='store_true',
                    help='overwrite extern/flow/include with this version')
    args = ap.parse_args()

    if sys.platform != 'win32':
        sys.exit('build_flow: only the Windows x64 build is set up')

    repo = os.path.abspath(args.repo)
    setup = os.path.join(repo, 'setup', 'flow')
    work = os.path.abspath(args.work or os.path.join(repo, 'build-flow'))
    os.makedirs(work, exist_ok=True)

    flow = fetch_flow(work)
    fetch_slang(work, flow)
    patch_source(flow, setup)
    bindir = build(work, flow, setup)

    for name, exports in EXPORTS.items():
        check_dll(os.path.join(bindir, name + '.dll'), exports)

    inc = os.path.join(repo, 'extern', 'flow', 'include')
    for h in HEADERS:
        src = os.path.join(flow, 'include', h)
        dst = os.path.join(inc, h)
        if os.path.isfile(dst) and not same_text(dst, src) and not args.update_headers:
            sys.exit('build_flow: %s differs from this Flow source; rerun with --update-headers '
                     'if the version bump is intended' % dst)
        if not os.path.isfile(dst) or args.update_headers:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
    lic = os.path.join(repo, 'extern', 'flow', 'LICENSE.md')
    if not os.path.isfile(lic):
        shutil.copy2(os.path.join(os.path.dirname(flow), 'LICENSE.md'), lic)

    for name in EXPORTS:
        dst = os.path.join(repo, name + '.dll')
        shutil.copy2(os.path.join(bindir, name + '.dll'), dst)
        log('wrote %s (%.1f MB)' % (dst, os.path.getsize(dst) / 1e6))
    return 0


if __name__ == '__main__':
    sys.exit(main())
