/*
Copyright (C) 2026 Q2RTX contributors

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

/*
==============================================================================

THE FROXEL GRID - dimensions and the froxel <-> world mapping.

Included by BOTH froxel.c and the two froxel shaders, so the CPU dispatch and
the GPU indexing cannot drift apart.

WHY A GRID AT ALL.  god_rays.comp marches every pixel through the medium and
evaluates the map's lights AND a sky-visibility ray at every step - roughly
1920x1080 half-res pixels times ~50 steps, so tens of millions of light
evaluations a frame.  That is why cl_fog 2 is both noisy (one jittered sample
per step, never reused) and slow.

A froxel grid evaluates the medium ONCE PER CELL of a view-frustum-aligned
volume instead, and the volume is small enough to reuse across frames.  At the
size below it is 901,120 cells - roughly a fortieth of the work - and because
each cell is a fixed point in view space, last frame's result can be reprojected
into this one, which is what actually removes the noise.

THE LAYOUT IS VIEW-FRUSTUM ALIGNED ("froxels", frustum voxels), not a world box:
x and y are screen space, so cell density follows the screen and no resolution is
wasted behind the camera or outside the view.  Z is distributed EXPONENTIALLY,
because perspective means near cells cover a few units and far cells cover
hundreds; a linear slice distribution would spend most of its cells on distance
nobody can resolve.

==============================================================================
*/

#ifndef FROXEL_SHARED_H_
#define FROXEL_SHARED_H_

// 160 x 88 is a 16:9-ish tile grid; the exact aspect does not have to match the
// screen because the froxel is looked up by NDC, not by pixel. 64 slices is the
// usual depth for this technique - fewer bands visibly on a wall edge.
// At RGBA16F that is 901120 * 8 = 7.2 MB per volume, and there are three.
#define FROXEL_GRID_X       160
#define FROXEL_GRID_Y       88
#define FROXEL_GRID_Z       64

// Thread group for the scatter pass: one thread per froxel.  8x8x1 keeps a
// group inside one screen tile so neighbouring threads hit the same cluster
// light list and the same region of the TLAS.
#define FROXEL_GROUP_X      8
#define FROXEL_GROUP_Y      8

// The integrate pass is one thread per COLUMN - it has to walk z in order.
#define FROXEL_INT_GROUP_X  8
#define FROXEL_INT_GROUP_Y  8

// The depth range the volume covers, in world units.  Past FROXEL_FAR the
// volume stops and god_rays_filter falls back to the far slice's transmittance,
// which is correct because by then the medium has saturated.
//
// FROXEL_NEAR is deliberately not tiny: the exponential distribution puts a
// third of the slices inside the first 5% of the range, and a near plane of a
// fraction of a unit wastes them on the inside of the player's own head.
#define FROXEL_NEAR         4.0
#define FROXEL_FAR          4096.0

/*
Slice <-> view depth.

  depth(s) = NEAR * (FAR/NEAR) ^ (s / GRID_Z)

so slice 0 sits at NEAR and slice GRID_Z at FAR, with each slice covering a
constant RATIO of depth rather than a constant amount.  The inverse is the log,
which is what the lookup in god_rays_filter.comp uses to find the slice for a
pixel's own view depth.

Written as macros rather than functions so the same text compiles as C and as
GLSL; both languages have log/pow/exp with these signatures.
*/
#define FROXEL_SLICE_TO_DEPTH(s) \
    (FROXEL_NEAR * pow(FROXEL_FAR / FROXEL_NEAR, (s) / float(FROXEL_GRID_Z)))

#define FROXEL_DEPTH_TO_SLICE(d) \
    (log((d) / FROXEL_NEAR) / log(FROXEL_FAR / FROXEL_NEAR) * float(FROXEL_GRID_Z))

#endif // FROXEL_SHARED_H_
