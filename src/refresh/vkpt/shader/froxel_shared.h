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
/*
WHICH AXIS TO RAISE, AND WHY IT IS NOT THE ONE YOU EXPECT.

These are compile-time because the volumes are allocated once from them and the
descriptor sets name those views; changing one needs a rebuild AND a vid_restart.
Memory is 3 volumes x X*Y*Z x RGBA16F (8 bytes), so the whole grid is ~21.6 MB at
the sizes below and cost is dominated by RAYS, not by storage.

**Z IS THE NOISE AXIS. X AND Y ARE THE DETAIL AXES.** Raising X/Y to fix noise is
the intuitive move and it does not work:

  A screen pixel's fog is the SUM ALONG ITS RAY of the cells it passes through -
  GRID_Z of them, whatever X and Y are. Each cell carries ONE light sample with
  TWO BINARY visibility rays. So the number of independent samples per pixel is
  set by Z ALONE. Doubling Z halves the variance (about 1.4x less noise) for 2x
  the rays. Doubling X and Y costs 4x the rays and leaves the per-pixel variance
  UNCHANGED - it only makes the noise finer-grained, and it makes things worse in
  one respect, because the spatial filter in froxel_integrate spans a fixed
  number of CELLS and therefore covers less world space as the cells shrink.

  What X/Y does buy is genuine spatial detail: a sharper edge where a shaft of
  sky light meets cover, and glow that pools tighter around a small fixture.
  That is a look decision, not a noise one.

Raise Z first, and reach for pt_fog_froxel_filter (a wider blur, no extra rays)
before either.
*/
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

/*
THE FAR PLANE MUST REACH AS FAR AS A PRIMARY RAY DOES, and 4096 did not.

primary_rays.rgen writes PRIMARY_RAY_T_MAX (10000) into PT_VIEW_DEPTH_A for
every pixel that hits NOTHING - i.e. every pixel showing sky.  The lookup in
god_rays_filter clamps anything past the far plane to the last slice, so with
the volume ending at 4096 a sky pixel received the fog from the first 4096
units and nothing for the remaining 5904.  The march has no such limit: it runs
to the edge of the world box.

The old comment here argued the clamp was harmless "because by then the medium
has saturated".  That is only true with real extinction; pt_fog_extinction
defaults to 0, which getDensity turns into a flat 0.0001, and at that rate a
ray is nowhere near saturated at 4096 - the shortfall is very close to linear.
So sky-facing pixels were getting roughly 40% of the fog the march gives them,
which is exactly the reported "the sky light fog has completely disappeared"
whenever the grid was on.

Tied to the constant rather than restated, so the two cannot drift apart.
*/
#define FROXEL_FAR          float(PRIMARY_RAY_T_MAX)

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

/*
==============================================================================

THE SCATTER VOLUME IS STORED PRE-MULTIPLIED, AND WITHOUT THIS THE WHOLE VOLUME
LIVES IN fp16 SUBNORMALS.

The scatter volume holds radiance PER UNIT LENGTH.  froxel_integrate multiplies
that by the slice thickness - tens to hundreds of world units - and accumulates
it over 64 slices before any of it reaches the image, so a single cell's value
is around three orders of magnitude below the composited fog.  The debug-view
gain in froxel_scatter.comp states the measured magnitude outright: composited
fog lands near 0.05 over a ~1000-unit ray, so the per-unit-length value is
about 5e-5.

fp16's SMALLEST NORMAL IS 6.1e-5.

So every cell of this volume was being stored below the format's normal range,
in the subnormals, where the mantissa is gone and all that is left is a fixed
absolute step of 2^-24 = 5.96e-8:

    a typical sky-lit cell   ~4e-6  ->    63 subnormal steps    ~6 bits
    the extinction channel   ~2e-6  ->    34 subnormal steps    ~5 bits

TWO CONSEQUENCES, AND THE SECOND ONE IS THE BUG.

1. Trilinear filtering and image stores are exactly where hardware is permitted
   to flush subnormals to zero, so the volume had a hard zero floor sitting in
   the middle of its own working range.

2. THE TEMPORAL BLEND CANNOT CLIMB.  The update is
   h <- 0.05*raw + 0.95*h, so each frame moves h by 5% of (raw - h).  At 63
   quantization levels that step is about three levels, and once h is within
   ~20 levels of raw the increment ROUNDS AWAY TO NOTHING and the exponential
   moving average STALLS - permanently, wherever it happens to be.

   That is the arithmetic contradiction this volume kept producing: every
   individual link measured sound, the round trip was verified exact, view 2 read
   green, views 7 and 8 agreed - and the volume still did not hold what the
   shader wrote.  It held what fp16 could represent of it, and the blend could
   not correct the difference because the correction was smaller than one step.

   It hits the SKY term hardest because the sky term is bordered by cells that
   are genuinely zero - rock overhead, or above the fog band - so a resampled
   history under camera motion lands low, and then cannot climb back.  A local
   light's glow is a smooth blob whose neighbours all carry signal, so its
   resampled history starts close to correct and the stall does not show.

THE FIX is to store the volume pre-multiplied and divide the factor back out on
load, which costs one multiply per store and one per load and moves the values
into the part of fp16 that has a mantissa.

CHOOSING THE FACTOR.  A power of two, so the multiply and divide are exact and
introduce no error of their own.  2^14 puts a typical 4e-6 cell at 0.066 - a
normal fp16 with the full 10-bit mantissa, about 16000 levels instead of 63 -
and leaves room for a value 4.0 before the format's 65504 ceiling, which is
roughly four thousand times the brightest cell the pt_fog_firefly clamp allows.
The clamp below is belt and braces: an Inf in this volume used to be permanent,
because the temporal blend fed it back into itself every frame.

Shared between froxel_scatter.comp (which scales on store) and
froxel_integrate.comp (which unscales on load) so the two cannot drift apart.
==============================================================================
*/
#define FROXEL_STORAGE_SCALE     16384.0
#define FROXEL_STORAGE_INV_SCALE (1.0 / FROXEL_STORAGE_SCALE)

// Just under fp16's 65504 maximum, so a scaled store can never become Inf.
#define FROXEL_STORAGE_MAX       60000.0

#endif // FROXEL_SHARED_H_
