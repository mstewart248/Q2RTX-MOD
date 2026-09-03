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

WHERE A FROXEL IS, AND WHERE IT TAKES ITS SAMPLE.

Extracted from froxel_scatter.comp so that the reservoir, spatial and scatter
passes cannot disagree about it, which they MUST NOT.  ReSTIR's bias correction
evaluates a neighbouring cell's target function at THAT CELL'S OWN sample point,
so if two passes reproduce a cell's jitter differently, the MIS denominator is
computed for a point that never contributed and the estimator quietly stops
being unbiased.  Nothing about the picture would say so - it would read as a
brightness error somebody would then chase with a scale cvar.

The includer must define rand(ivec2) before including this; froxel_shared.h and
constants.h must already be included.

==============================================================================
*/

#ifndef FROXEL_SAMPLE_GLSL_
#define FROXEL_SAMPLE_GLSL_

/*
Froxel centre -> world position.

x and y are NDC across the screen; z is the exponential depth slice.  The jitter
moves the sample point inside its own cell every frame, so what the temporal
blend accumulates is a stratified estimate of the cell rather than the same
corner over and over.  Without it the volume converges to a grid of point
samples and the banding stays.
*/
vec3 froxel_to_world(ivec3 cell, vec3 jitter)
{
	vec2 ndc = ((vec2(cell.xy) + jitter.xy) / vec2(FROXEL_GRID_X, FROXEL_GRID_Y)) * 2.0 - 1.0;

	float view_depth = FROXEL_SLICE_TO_DEPTH(float(cell.z) + jitter.z);

	/* Unproject to a view-space ray, then push it out to view_depth.

	   TWO SIGN FACTS ABOUT THIS TREE, both settled by reading matrix.c rather
	   than by convention:

	   1. QUAKE II RTX'S VIEW SPACE IS +Z FORWARD.  create_view_matrix() puts
	      viewaxis[0] - the FORWARD axis - in the matrix's z row, so
	      view.z = dot(forward, world - vieworg), positive in front of the eye.
	      create_projection_matrix() sets matrix[11] = 1, so clip.w = view.z,
	      positive in front as well.

	   2. invP STILL HANDS BACK A POINT WITH NEGATIVE z HERE.  The projection's
	      z row is vestigial in a path tracer - projection.glsl only ever uses
	      clip.xy/clip.w and computes distance separately - and solving
	      clip.z == 0 against it gives view.z = -2fn/(f+n), which is negative.

	   So the unprojected point really does come back behind the camera, and the
	   old `view_dir /= max(abs(view_dir.z), 1e-6)` did NOT correct for that:
	   dividing by the MAGNITUDE leaves z at -1, so every froxel in the grid was
	   placed behind the eye AND mirrored in x and y (dividing by a negative
	   number flips the x/y signs too).  The whole volume sampled the medium at
	   positions reflected through the camera - wrong densities, wrong shadow-map
	   texels, wrong distances to every light.

	   Negating the whole vector is the correct repair and preserves the NDC it
	   was built from: ndc.x = (P00*vx + P20*vz)/vz is a ratio of linear terms,
	   so it is invariant under scaling by ANY non-zero factor, negative included.

	   The ray is then NORMALIZED rather than scaled to z == 1, which makes this
	   grid's z axis a RADIAL distance.  That is deliberate: god_rays_filter.comp
	   looks the volume up with PT_VIEW_DEPTH_A, and primary_rays.rgen writes that
	   from projection_view_to_screen(), whose `distance` output is
	   length(view_pos) - radial, not planar.  Building the grid on planar depth
	   while reading it back with a radial one sampled the wrong slice everywhere
	   except dead centre, by up to ~1.4x out at the screen corners. */
	vec4 view_h = global_ubo.invP * vec4(ndc, 0.0, 1.0);
	vec3 view_dir = view_h.xyz / view_h.w;

	if (view_dir.z < 0.0)
		view_dir = -view_dir;

	view_dir = normalize(view_dir);

	vec3 view_pos = view_dir * view_depth;

	return (global_ubo.invV * vec4(view_pos, 1.0)).xyz;
}

/*
Everything about one cell that more than one pass needs, derived the SAME way in
each of them.

`p` is the jittered point - where the LIGHTING is evaluated, and the point whose
target function ReSTIR must use for that cell.  `p_centre` is the cell centre,
which is what density, the fog albedo, the sky ray and the history lookup use;
see the three long notes in froxel_scatter.comp for why those must not be
jittered.
*/
struct FroxelCellSample
{
	vec3 p;
	vec3 p_centre;
	vec3 view_dir;
	vec3 light_rnd;   // .x picks the light, .yz place the point on it
	uint seed;
};

/* base_rand is ONE blue-noise value for the whole column, passed in rather than
   fetched here so the scatter pass can keep hoisting it out of its z loop -
   sampling the texture once and stepping it is much cheaper than 128 fetches,
   and the sequence only has to decorrelate neighbours. */
FroxelCellSample froxel_cell_sample(ivec3 cell, float base_rand)
{
	FroxelCellSample s;

	// Stepped per slice by three different golden-ratio increments.
	float r1 = fract(base_rand + float(cell.z) * 0.6180339887);
	float r2 = fract(base_rand + float(cell.z) * 0.3247179572);
	float r3 = fract(base_rand + float(cell.z) * 0.7548776662);

	s.p        = froxel_to_world(cell, vec3(r1, r2, r3));
	s.p_centre = froxel_to_world(cell, vec3(0.5));

	vec3 to_cell = s.p - global_ubo.cam_pos.xyz;
	float cell_dist = length(to_cell);
	s.view_dir = (cell_dist > 1e-4) ? to_cell / cell_dist : vec3(0, 0, 1);

	/* r2 and r3 CANNOT be used to place a point on an area light, even though
	   they are perfectly good for picking which light. They come from ONE
	   blue-noise fetch stepped by two different golden-ratio increments, so for
	   a fixed slice the pair (r2, r3) is a deterministic function of the single
	   value base_rand: as it varies across the screen the pair traces a LINE
	   through the unit square rather than filling it. Feeding that to
	   sample_projected_triangle would sample every light along one diagonal of
	   its surface and show up as structure, not as noise a filter can remove.

	   Two independent fetches instead. The offsets are arbitrary and just have
	   to land somewhere uncorrelated in the blue-noise texture; z is folded in
	   so the samples also decorrelate down the column. */
	s.light_rnd = vec3(r1,
		rand(cell.xy + ivec2(cell.z * 29 + 101, cell.z * 17 + 53)),
		rand(cell.xy + ivec2(cell.z * 41 + 197, cell.z * 23 + 149)));

	s.seed = uint(cell.x) * 73856093u
	       ^ uint(cell.y) * 19349663u
	       ^ uint(cell.z) * 83492791u
	       ^ uint(global_ubo.current_frame_idx) * 2654435761u;

	return s;
}

/*
==============================================================================

THE RESERVOIR VOLUMES.

Two of them, because spatial reuse reads every neighbour's PRE-reuse reservoir
while writing its own - doing that in place is a race whose result depends on
dispatch order.  froxel_reservoir.comp writes A, froxel_spatial.comp reads A and
writes B, froxel_scatter.comp reads B.

RGBA32F, NOT RGBA16F, and that is not caution for its own sake: `.x` holds a
light INDEX and fp16 represents integers exactly only up to 2048, while `.y`
holds the RIS weight W, whose whole job is to be large exactly when the target
function was small.  This volume has no equivalent of FROXEL_STORAGE_SCALE to
rescue it, and the fp16 subnormal bug in the scatter volume is the standing
reminder of what that costs.

  .x  chosen light index as a float, or -1 for "this reservoir holds no sample"
  .y  W, the RIS weight - the unbiased contribution weight of that sample
  .z  M, how many candidate draws this reservoir represents (0 = cell not lit)
  .w  the target function of .x evaluated at the cell's OWN sample point

.w is what lets a neighbour reconstruct this reservoir's unnormalised weight
p_hat * W * M without re-deriving it, and it is the value the spatial pass seeds
its own stream with.
==============================================================================
*/
#define FROXEL_RESERVOIR_EMPTY vec4(-1.0, 0.0, 0.0, 0.0)

#endif // FROXEL_SAMPLE_GLSL_
