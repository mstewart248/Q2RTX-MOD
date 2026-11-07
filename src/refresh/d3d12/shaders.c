/*
D3D12 shaders: the DXIL the build makes from src/refresh/vkpt/shader.

CMake compiles every Vulkan shader a second time with -DVKPT_D3D12 and runs
the SPIR-V through spv2dxil (SPIRV-Cross to HLSL, then DXC to signed DXIL), so
baseq2/shader_d3d12/<name>.dxil sits beside baseq2/shader_vkpt/<name>.spv for
the same <name>: stretch_pic.vert, asvgf_atrous.comp, primary_rays.rgen.query...
*/

#include "d3d12pt.h"

void *d3d12_load_shader(const char *name, size_t *size)
{
    char path[MAX_QPATH];
    if (Q_snprintf(path, sizeof(path), "shader_d3d12/%s.dxil", name) >= sizeof(path)) {
        Com_EPrintf("D3D12: shader name too long: %s\n", name);
        return NULL;
    }

    void *data = NULL;
    int len = FS_LoadFile(path, &data);
    if (!data || len <= 0) {
        Com_EPrintf("D3D12: couldn't load %s - built with CONFIG_D3D12_RENDERER? (shaders_d3d12 target)\n", path);
        return NULL;
    }
    *size = (size_t)len;
    return data;
}
