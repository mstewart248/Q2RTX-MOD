/*
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef _SHADER_PHYSICAL_SKY_
#define _SHADER_PHYSICAL_SKY_

#define PHYSICAL_SKY_FLAG_NONE            (0)

#define PHYSICAL_SKY_FLAG_USE_SKYBOX      (1 << 0)
#define PHYSICAL_SKY_FLAG_DRAW_MOUNTAINS  (1 << 1)
#define PHYSICAL_SKY_FLAG_DRAW_CLOUDS     (1 << 2)

#define PHYSICAL_SKY_MASK_OFFSET (PHYSICAL_SKY_FLAG_DRAW_CLOUDS - 1)

/* physical_sky(_space).comp's rows, counted from global_ubo.sky_first_row
   (vkpt_sky_work_t), 1024 texels each. Rows 0 .. SKY_ENVMAP_ROWS-1 are the six
   faces' rows in order: the whole sky in one dispatch. Rows SKY_ENVMAP_ROWS and
   up are the same texels in the order a sliced update renders them
   (sky_update_slices): a 4x4 ordered dither over the whole cube, so a part-done
   update is a fine even mix of the old sky and the new rather than some faces
   new and the rest old (Matt 11-24: the cube's corners showing and the faces
   flickering in turn as the sun came up). See sky_envmap_texel. */
#define SKY_ENVMAP_RES      1024
#define SKY_ENVMAP_ROWS     (6 * SKY_ENVMAP_RES)
#define SKY_DITHER_RANKS    16      // texels of a 4x4 tile, one rank each


#ifndef VKPT_SHADER



#else
bool useSkybox() {
    return (global_ubo.physical_sky_flags & PHYSICAL_SKY_FLAG_USE_SKYBOX) != 0;
}

bool drawMountains() {
    return (global_ubo.physical_sky_flags & PHYSICAL_SKY_FLAG_DRAW_MOUNTAINS) != 0;
}

bool drawClouds() {
    return (global_ubo.physical_sky_flags & PHYSICAL_SKY_FLAG_DRAW_CLOUDS) != 0;
}

/* The texel (x, y, face) a physical_sky(_space).comp thread renders, from its
   global id and global_ubo.sky_first_row; out of range (>= res, res, 6) for a
   thread past the end. See SKY_ENVMAP_ROWS above.

   The dithered order is rank-major: a rank is one texel of every 4x4 tile, in
   the Bayer order, so any run of ranks is spread evenly over the cube. Inside a
   rank, a 16x16 group takes a 16x16 block of tiles (a 64x64 texel square), so
   neighbouring threads still march neighbouring rays. A slice that ends partway
   through a rank leaves a part of that one rank behind: 1/16 of the texels. */
ivec3 sky_envmap_texel(uvec3 gid, int first_row)
{
    const int res = SKY_ENVMAP_RES;
    int row = int(gid.z) * res + int(gid.y) + first_row;

    if (row < SKY_ENVMAP_ROWS)
        return ivec3(int(gid.x), row % res, row / res);

    row -= SKY_ENVMAP_ROWS;
    const int rank_rows = SKY_ENVMAP_ROWS / SKY_DITHER_RANKS;  // 384
    const int rank = row / rank_rows;
    if (rank >= SKY_DITHER_RANKS)
        return ivec3(res, res, 6);
    row %= rank_rows;

    const int side = res / 4 / 16;                      // 16 blocks of 16 tiles a face's side
    const int block = (row / 16) * (res / 16) + int(gid.x) / 16;
    const int face = block / (side * side);
    const int bf = block % (side * side);
    const ivec2 tile = ivec2(bf % side, bf / side) * 16 + ivec2(int(gid.x) % 16, row % 16);

    // the 4x4 Bayer matrix's position of `rank`: bit 0 -> (2,2), 1 -> (2,0),
    // 2 -> (1,1), 3 -> (1,0), combined by XOR
    const ivec2 o = ivec2(((rank ^ (rank >> 1)) & 1) * 2 + (((rank >> 2) ^ (rank >> 3)) & 1),
                          (rank & 1) * 2 + ((rank >> 2) & 1));

    return ivec3(tile * 4 + o, face);
}
#endif

#endif // _SHADER_PHYSICAL_SKY_
