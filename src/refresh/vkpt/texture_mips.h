/*
vkpt/textures.c's mip chain rules, API-free, so the D3D12 renderer
(d3d12/textures.c) builds the same chains: how many levels an image gets, and
which images get the max-filtered chain instead of the linear one.
*/

#pragma once

#include "shared/shared.h"
#include "refresh/images.h"

// 1 + log2 of the larger side; 1 for IF_NO_MIPMAPS (video frames)
int vkpt_image_num_miplevels(const image_t *q_img);

// POM height maps (IF_HEIGHT_MAP) with more than one level: a chain of maximum heights
bool vkpt_image_has_max_mips(const image_t *q_img);

// one level of that chain, `src` wd x ht into `dst` nwd x nht, `bpp` 4 or 2
void vkpt_downsample_max(const byte *src, int wd, int ht, byte *dst, int nwd, int nht, int bpp);
