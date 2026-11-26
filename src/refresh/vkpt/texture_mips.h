/*
vkpt/textures.c's mip chain rules, API-free, so the D3D12 renderer
(d3d12/textures.c) builds the same chains: how many levels an image gets, and
which images get the max-filtered chain instead of the linear one.
*/

#pragma once

#include "shared/shared.h"
#include "refresh/images.h"

// 1 + log2 of the larger side - of the texture as uploaded, so times
// vkpt_image_upload_scale; 1 for IF_NO_MIPMAPS (video frames)
int vkpt_image_num_miplevels(const image_t *q_img);

/* THE NEAREST-NEIGHBOUR UPSCALE (image_t upload_scale, from the materials'
   rough_scale / metal_scale / emissive_scale). The GPU texture is pix_data with
   every texel repeated N x N; pix_data itself stays as loaded. 1 for an image
   whose chain is built from pix_data's own texels in a way an upscale would
   break - a height map's max-filtered chain, a normal map's renormalised level
   0 - and for IF_NO_MIPMAPS; and lowered so neither side passes
   VKPT_UPLOAD_SCALE_MAX_SIDE. Its level 0 is (upload_width x upload_height)
   times this. */
#define VKPT_UPLOAD_SCALE_MAX       4
#define VKPT_UPLOAD_SCALE_MAX_SIDE  4096
int vkpt_image_upload_scale(const image_t *q_img);

// (src), w x h texels of (bpp) bytes, with every texel made a (scale) x (scale)
// block, into (dst): (w * scale) x (h * scale), tightly packed. scale 1 = a copy.
void vkpt_image_expand_nearest(const byte *src, int w, int h, int bpp, int scale, byte *dst);

// POM height maps (IF_HEIGHT_MAP) with more than one level: a chain of maximum heights
bool vkpt_image_has_max_mips(const image_t *q_img);

// one level of that chain, `src` wd x ht into `dst` nwd x nht, `bpp` 4 or 2
void vkpt_downsample_max(const byte *src, int wd, int ht, byte *dst, int nwd, int nht, int bpp);
