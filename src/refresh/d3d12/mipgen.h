/*
D3D12 texture mip chains on the CPU (mipgen.c). Engine-free: textures.c calls it,
tests/d3d12_mip_test.c includes the .c.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One level down, as vkpt's linear vkCmdBlitImage makes it: `src` is wd x ht, `dst`
// nwd x nht, both tightly packed, `bpp` 4 (RGBA8; sRGB colour when `srgb`) or 2 (R16)
void d3d12_mip_downsample(const uint8_t *src, int wd, int ht, uint8_t *dst, int nwd, int nht, int bpp, bool srgb);

// normalize_normal_map.comp on `texels` RGBA8 texels, in place
void d3d12_mip_normalize_normals(uint8_t *rgba, size_t texels);
