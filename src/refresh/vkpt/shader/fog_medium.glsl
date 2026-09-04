/*
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
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

THE FOG MEDIUM - density, colour and in-scatter at a world point.

Extracted verbatim from god_rays.comp so the froxel grid can evaluate the same
medium.  Nothing here changed in the move; the comments are the originals and
they carry the reasoning for every constant, most of which was settled by
measurement rather than derivation.  Read them before touching a number.

The includer must already have brought in, in this order:

    constants.h, utils.glsl
    god_rays_shared.h   (for FOG_TLAS)
    global_ubo.h        (for global_ubo)
    vertex_buffer.h     (for light_buffer / get_light_polygon)
    global_textures.h

and must define `rand(ivec2)`, which both callers do because they seed it
differently - the march wants a per-step sequence, the froxel pass a per-cell
one.

==============================================================================
*/

#ifndef FOG_MEDIUM_GLSL_
#define FOG_MEDIUM_GLSL_

// Drawing a point on a light, and its solid-angle pdf, for all three light
// types. This is the normal-free half of light_lists.h, split out precisely so
// a volume can use it - see the header comment there. It replaced a local copy
// of spotlight_falloff that had been made here for want of it.
#include "light_sampling.h"

// How thick the rerelease's authored densities render, relative to the flat
// medium this pass used before. Their heightfog_density runs 0.0001..0.0024 and
// their fog_density 0.01..0.05, so both are normalised into roughly 0..1 here
// before the world-box falloff is applied. This constant is the one thing in
// the fog path that is a judgement call rather than map data - it stands in for
// the density-to-extinction constant inside the closed KEX renderer, which we
// cannot read. Tune with cl_fog_scale, not by editing this.
// CALIBRATED IN PLAY on base1, which is the only way this could be settled -
// KEX's density-to-extinction constant is inside a closed renderer.
//
// The history is worth keeping because it was wrong twice in opposite
// directions. The first values (0.0015 / 0.03) made a typical map EIGHT TIMES
// THINNER than the flat 1.0 medium this pass used before, so the fog read as
// "no difference". Correcting to 0.00025 / 0.02 put base1 at density 1.80 -
// which then turned out to be 100x TOO THICK once local lights were actually
// scattering, because density scales the in-scatter linearly and the earlier
// numbers were matched against a sun-lit medium with no local lights in it.
//
// Matt landed on cl_fog_scale 0.01 with pt_fog_light_scale 1, i.e. an effective
// base1 density of ~0.018. These constants bake that in so cl_fog_scale 1.0 is
// the value that looks right, which is what the cvar claims to mean.
#define FOG_HEIGHTFOG_REFERENCE 0.025
#define FOG_DISTANCEFOG_REFERENCE 2.0

// How much total decay is allowed from the bottom of the band to the top.
//
// heightfog_falloff CANNOT be used as a per-world-unit exponent, which is what
// this function did at first and it was wrong. Multiplied out over a real band
// the exponent is enormous - mgu5m1 is falloff 0.029 over a 1200-unit band, so
// exp(-34.8) = 7.7e-16 and the fog is annihilated everywhere except exactly on
// the floor plane. Measured across the maps the exponent runs 1.08 to 34.8, and
// ONLY the two maps with a freak 21-unit band (mgu6m1, mgu6m2) land low enough
// to survive. That is exactly the symptom Matt saw: fog on mgu6m1, none on
// mgu5m1.
//
// So the falloff is normalised over the band instead, and clamped. That keeps
// the shape the author intended - dense at the ground, thinning upward - while
// guaranteeing the band is actually populated. The two maps that already worked
// sit inside the clamp and are unchanged by it.
// MAX lowered from 4.0 to 1.5 after measuring where the player actually stands
// in each band. At 4.0 a player 60% of the way up loses 91% of the density -
// which is what made mgu5m1 invisible: it has NO distance fog term at all, only
// height fog, so that decay was the whole story there. base1 barely notices the
// change because its fog is carried by fog_density, which has no height falloff.
#define FOG_MIN_BAND_DECAY 0.75
#define FOG_MAX_BAND_DECAY 1.5

// The altitude-banded part. start_z is the TOP of the band and end_z the
// BOTTOM (id's key names say "dist" but they are world Z heights), so the fog
// is thickest at the bottom and thins upward.
float getHeightFogDensity(vec3 p)
{
	if (global_ubo.fog_hf_density <= 0)
		return 0;

	float height_above_floor = p.z - global_ubo.fog_hf_end_z;

	// solid below the band, so a pit or a low room fills completely
	if (height_above_floor <= 0)
		return global_ubo.fog_hf_density / FOG_HEIGHTFOG_REFERENCE;

	float band = global_ubo.fog_hf_start_z - global_ubo.fog_hf_end_z;
	if (band <= 0)
		return global_ubo.fog_hf_density / FOG_HEIGHTFOG_REFERENCE;

	// height through the band, 0 at the floor and 1 at the top
	float t = height_above_floor / band;

	float decay = clamp(global_ubo.fog_hf_falloff * band,
	                    FOG_MIN_BAND_DECAY, FOG_MAX_BAND_DECAY);

	float d = global_ubo.fog_hf_density * exp(-t * decay);

	// and taper to nothing above the top of the band, so it does not fill the sky
	d *= clamp(1.0 - (t - 1.0), 0.0, 1.0);

	return d / FOG_HEIGHTFOG_REFERENCE;
}

// How many of the frame's model lights one march step looks at. The rerelease
// maps submit up to MAX_DLIGHTS (256) dynamic_lights per frame and this pass
// marches ~50 steps per ray, so looking at all of them everywhere is far too
// much work. Instead each step takes a strided subset and scales the result
// back up - the same stochastic trick sample_light_list uses - and the existing
// bilateral filter plus the denoiser absorb the resulting noise.
#define FOG_LIGHT_SAMPLES 8

/* DIAGNOSTIC TAPS for pt_fog_froxel_debug. Written by the medium as it goes and
   read by nothing else, so they cost a register and no bandwidth.

   fog_debug_sky_term used to be declared further down, next to
   getVolumeLightInscatter, which meant ONLY MODE 3 ever wrote it - so
   pt_fog_froxel_debug 4 read solid black under cl_fog 2 whether or not the sky
   term was actually there. A diagnostic that returns the "it is broken" answer
   for a configuration it does not measure is worse than no diagnostic, because
   the reading is indistinguishable from the real fault it was built to find.
   Both in-scatter functions write it now.

   fog_debug_sky_vis is the RAW visibility, before any radiance, scale or albedo
   touches it. That is the one number that separates "the grid's cells cannot see
   the sky at all" from "they can, and the term is being lost somewhere
   downstream of getSkyVisibility" - which is the fork the whole investigation is
   stuck on. */
vec3  fog_debug_sky_term = vec3(0);
float fog_debug_sky_vis  = 0.0;

/*
=================
getSkyVisibility

TRUE per-point sky exposure, by tracing. This is the thing no cluster mask could
give: sky_cluster_mask (clusters containing sky faces) missed an open canyon
floor because the sky brushes are overhead in another cluster, while
sky_visibility (the PVS union) filled caves that merely had a sight line out.
Both are per-cluster, so both were constant across a whole room and could not
tell one part of it from another. That is also why fog never pooled under a hole
in a roof - the whole room shared one answer.

One ray, straight up. If it escapes the geometry, this point sees sky. Cheap
because it is an occlusion query with terminate-on-first-hit and no closest-hit
work, and because it only runs where the fog is dense enough to matter.

Straight up rather than a proper hemisphere integral: sampling several
directions per march step would be the honest version, but this pass is already
per-pixel-per-step and one ray is what makes it affordable. It gets the shape
right - open ground bright, under an overhang dark, a shaft under an opening -
which is what the per-cluster version could not do at all.
=================
*/
float getSkyVisibility(vec3 p)
{
	if (global_ubo.fog_sky_trace == 0)
	{
		// fall back to the CPU cluster estimate
		fog_debug_sky_vis = global_ubo.fog_sky_fade;
		return fog_debug_sky_vis;
	}

	/* BINARY VISIBILITY IS WHAT MAKES THIS LOOK WRONG, and it causes two
	   separate complaints that turn out to be the same defect.

	   Returning a hard 0 or 1 means a march point either gets the WHOLE sky term
	   or none of it. Outdoors that paints the open air above a roof at full
	   strength and the volume under the roof at zero, with the boundary tracing
	   the building's silhouette - the blocky rectangular patches of fog hanging
	   over a base with no light anywhere near them.

	   It is also the froxel grid's noise. A cell's sample point is JITTERED
	   inside the cell every frame; when that jitter carries it across the
	   visibility boundary the term flips between full sky and nothing, and the
	   temporal blend turns that flicker into mottling. The grid looks noisier
	   than the march here not because the grid is worse but because it samples
	   the same step function more coarsely.

	   pt_fog_sky_soften > 0 replaces the step with a ramp: take the CLOSEST hit
	   rather than any hit, and fade the sky in over that many world units of
	   clearance. A point just under a ceiling gets nothing, a point in a tall
	   shaft gets most of the sky, open air gets all of it. It is not a real
	   hemisphere integral - it is one ray - but it is continuous, so it has no
	   hard edge to alias against and no step for the jitter to flip across,
	   which removes the added noise rather than trading it.

	   Note the flags: the closest hit needs the TERMINATE-ON-FIRST-HIT flag
	   GONE, because with it committed t is whatever was found first, not the
	   nearest. 0 keeps the original any-hit binary test, which is cheaper. */
	float soften = global_ubo.pt_fog_sky_soften;

	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, FOG_TLAS,
		(soften > 0.0) ? gl_RayFlagsOpaqueEXT
		               : (gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT),
		AS_FLAG_OPAQUE,
		p, 1.0, vec3(0, 0, 1), 65536.0);

	while (rayQueryProceedEXT(rq)) {}

	bool blocked = (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT);

	if (!blocked)
		fog_debug_sky_vis = 1.0;
	else if (soften <= 0.0)
		fog_debug_sky_vis = 0.0;
	else
		fog_debug_sky_vis = clamp(rayQueryGetIntersectionTEXT(rq, true) / soften, 0.0, 1.0);

	return fog_debug_sky_vis;
}

// Hemisphere solid angle for the sky term. Kept separate from the local-light
// path so the two can be balanced independently if the sky turns out to
// dominate outdoors - open-air maps see this from nearly every march point,
// where a local light only reaches a few.
#define FOG_SKY_SOLID_ANGLE 3.14159

// 1 / (4 pi). Anywhere the fog scatters light that has no preferred direction -
// the local-light term below, and the ambient-in-shadow term in the march.
#define FOG_ISOTROPIC_PHASE 0.0796

// In-scatter at a point from the map's own lights.
//
// This reads the BSP CLUSTER LIGHT LIST, not a raw index range, and that choice
// fixes three things at once. Reading only
// [num_static_lights, +num_model_lights) - which is what this did first - meant
// the loop saw ONLY dynamic lights: every emissive surface and the sky light
// were excluded, so nothing in a normal map lit the fog at all, and a thrown
// flare was the only thing that ever showed up. The cluster list holds the
// static lights AND the model lights (inject_model_lights puts them straight
// into the visibility lists), so both now contribute.
//
// It is also PVS-culled, which is what stops one bright light washing the whole
// level. That mattered more when these samples were unshadowed: without the PVS
// restriction a single flare lit fog through every wall in the map. It is still
// the cheap first cut, but pt_fog_light_shadow now traces real occlusion, which
// is what PVS could never do - PVS is per-CLUSTER, so inside the camera's own
// cluster a fixture lit fog straight through walls, floors and roofs.
//
// EVERY light type in the port is evaluated here: emissive surfaces
// (DYNLIGHT_POLYGON), and the dlights - weapon fire, muzzle flashes, flares,
// target_light, the flashlight, the rerelease dynamic_light entities and the
// lights placed with `light place` - as DYNLIGHT_SPHERE / DYNLIGHT_SPOT. The
// three geometries are unified into a solid angle in the loop below; see the
// long note there, and note that a SPHERE's positions[] are NOT vertices.
//
// How much each one fogs is its volumetric_scale, resolved on the CPU in
// copy_light() from an explicit per-light value, then the material's, then the
// pt_fog_scale_* default for its class. That is RTX Remix's per-light
// volumetricRadianceScale, and it is the knob that replaced the blunt
// "model lights contribute nothing" rule this used to have.
//
// The cluster is the one at the surface the ray ends on, not at each march
// point. That is an approximation, but it is the right region's light set and it
// costs one texture fetch instead of a per-step BSP walk.
/* sky_p is where the SKY VISIBILITY RAY starts, and it is deliberately separate
   from p, where the LIGHTING is evaluated.

   The froxel grid jitters its sample point inside the cell every frame to
   stratify the lighting integral - which light is chosen, where on it the sample
   lands, whether the shadow ray is blocked. Those are integrals and they need
   the jitter. getSkyVisibility is NOT an integral: it is a single ray straight
   up, a point query with a binary answer. Jittering it buys no variance
   reduction at all and costs the distant fog, because a far froxel spans
   hundreds of units laterally, so the up-ray clears an opening one frame and
   hits rock the next. The cell's sky term becomes a coin flip that the temporal
   blend cannot hold, while the light term - whose visibility ray goes to a
   nearby chosen light - stays stable. That is exactly why the fog loss was
   reported as affecting the sky light and nothing else.

   The march passes the same point for both, since it has no cell to jitter
   within. */
vec3 getClusterLightInscatter(uint cluster_idx, vec3 p, vec3 sky_p, float rand01)
{
	// THE SKY TERM IS COMPUTED FIRST, and deliberately so. It used to sit at the
	// bottom of this function, after two early returns - one for an invalid
	// cluster and one for a cluster holding no light polygons. That meant an
	// open area with no emissive surfaces near it got NO SKY AT ALL, which is
	// precisely a canyon floor in mgu5m1: standing under open sky with nothing
	// emissive in the cluster, and the fog vanished. Sky exposure has nothing to
	// do with how many light polys a cluster happens to contain.
	/* WHICH SKY THE FOG IS ALLOWED TO SEE. Getting this wrong is what put a
	   blazing orange slab in a NIGHT-lit map.

	   fog_sky_* is `avg_envmap_color * pt_env_scale`, the average of the MAP's
	   skybox images, computed once at map load in R_SetSky_RTX. It is the right
	   answer only when the map actually ships a skybox.

	   When the PHYSICAL sky is in use there is no skybox to average, and
	   R_SetSky_RTX falls back to a 1x1 magenta cube whose average works out to
	   (1.0, 1.0, 0.0) - full-intensity yellow. The fog then lit itself with that
	   constant no matter what time of day the physical sky was set to, so a
	   night sky produced the same searing sky-fog as noon. Tinted by the map's
	   own fog colour it comes out orange, which is exactly the flat slab Matt
	   photographed pouring through an opening in an otherwise dark room.

	   sun_color_ubo.sky_color is what physical_sky.comp accumulates from the sky
	   it actually rendered, so it tracks sun_elevation / the presets. It is zero
	   on maps that ship their own skybox, which is why the two cases have to be
	   told apart rather than one used for both.

	   ENVIRONMENT_NONE means no world sky at all - contributing the fallback
	   there was never anything but a bug. */
	vec3 sky_term = vec3(0);
	float sky_vis = getSkyVisibility(sky_p);
	if (sky_vis > 0.0)
	{
		vec3 sky_radiance = vec3(0);

		if (global_ubo.environment_type == ENVIRONMENT_DYNAMIC)
			sky_radiance = sun_color_ubo.sky_color;
		else if (global_ubo.environment_type == ENVIRONMENT_STATIC)
			sky_radiance = vec3(global_ubo.fog_sky_r,
			                    global_ubo.fog_sky_g,
			                    global_ubo.fog_sky_b);

		sky_term = sky_radiance
		         * FOG_SKY_SOLID_ANGLE * global_ubo.pt_fog_sky_scale
		         * sky_vis;
	}

	// The caller applies FOG_ISOTROPIC_PHASE to the whole return value in this
	// mode, where mode 3 folds it into its own sky_term - so scale by it here to
	// make the tap read the same quantity in both modes and keep debug 4
	// comparable across a cl_fog 2 / 3 A/B.
	fog_debug_sky_term = sky_term * FOG_ISOTROPIC_PHASE;

	// Same isolation as mode 3 - see the note there. Kept in both in-scatter
	// functions so the cvar means the same thing on the march and on the grid,
	// which is what makes cl_fog 2 usable as the reference it is supposed to be.
	int fog_isolate = int(global_ubo.pt_fog_isolate);
	if (fog_isolate == 2)
	{
		sky_term = vec3(0);
		fog_debug_sky_term = sky_term;
	}
	else if (fog_isolate == 1)
		return sky_term;

	if (cluster_idx == ~0u)
		return sky_term;

	uint list_start = light_buffer.light_list_offsets[cluster_idx];
	uint list_end   = light_buffer.light_list_offsets[cluster_idx + 1];

	uint count = list_end - list_start;
	if (count == 0)
		return sky_term;

	/* HOW MANY OF THE CLUSTER'S LIGHTS THIS STEP LOOKS AT.
	   Raising this is the direct trade of frame time for fog noise, and it
	   matters far more once pt_fog_light_falloff is steep - see the variance
	   note at the count/taken rescale below. */
	uint taken  = min(count, uint(max(global_ubo.pt_fog_light_samples, 1.0)));
	uint stride = max(1u, count / taken);
	uint offset = uint(rand01 * float(count)) % count;

	vec3 sum = vec3(0);

	for (uint i = 0; i < taken; i++)
	{
		uint n_idx = list_start + ((offset + i * stride) % count);
		uint light_idx = light_buffer.light_list_lights[n_idx];
		if (light_idx == ~0u)
			continue;

		LightPolygon light = get_light_polygon(light_idx);

		/* THE SKY IS SKIPPED FIRST, because everything below reads light.color
		   and the sky's is a MARKER, not a colour.

		   copy_light() marks a sky polygon by storing its colour NEGATIVE
		   (`-sky_radiance * 0.5`, vertex_buffer.c). The old line further down
		   was `vec3 color = abs(light.color);` - which does not honour the
		   marker, it ERASES it, turning every sky brush face in the cluster
		   into a perfectly ordinary bright area light.

		   That is ruinous for fog specifically, because `area` is folded into
		   the sum below and a sky brush face is enormous - thousands of square
		   units against a light fixture's tens. So the sky brushes dominated
		   the local-light sum near the top of a map, which is exactly where
		   they are, and produced glowing fog above the rooftops with no fixture
		   anywhere near it.

		   Every symptom followed from this and none of it responded to the
		   knobs:
		     - pt_fog_sky_scale did nothing, because that scales sky_term, and
		       this was arriving through the LOCAL LIGHT path instead;
		     - pt_fog_light_scale killed it, because that scales this sum;
		     - pt_fog_light_shadow did not help, because a sky brush genuinely IS
		       visible from open air - the occlusion test was answering honestly;
		     - setting the physical sky to NIGHT did not dim it, because
		       sky_radiance comes from avg_envmap_color, which is fixed at map
		       load from the skybox images and knows nothing about time of day.

		   The sky's contribution to fog belongs to sky_term at the top of this
		   function, which has its own visibility trace and its own scale.
		   Skipping the marked polygons here accounts for it exactly once. */
		if (any(lessThan(light.color, vec3(0.0))))
			continue;

		/* EVERY LIGHT IN THE PORT REACHES HERE NOW, AND THAT IS THE CHANGE.

		   This used to be `if (light_idx >= num_static_lights) continue;`, which
		   dropped every MODEL light - and model lights are not a minor category.
		   They are, in this tree: weapon fire and muzzle flashes (EF_BLASTER and
		   friends, V_AddLight, always on regardless of cl_dynamic_lights),
		   misc_flare, target_light, the flashlight, beams and lasers, emissive
		   surfaces on md2/md5 models, the rerelease dynamic_light entities, and
		   the lights placed by hand with `light place`. So a placed light lit
		   the room and made no fog at all, which is exactly the symptom that
		   started this.

		   The original reason for the skip was real: a point light has no extent
		   for the area term to damp, and NOTHING here was shadowed, so one
		   bright dlight lit fog through the whole map. Both halves of that have
		   since gone - pt_fog_light_shadow traces occlusion, and each light now
		   contributes its own true solid angle rather than a triangle's area.
		   What is left is the per-light volumetric scale, which is the honest
		   control: a light that should not fog gets 0, by class default or
		   individually.

		   THE THREE LIGHT TYPES DO NOT SHARE A GEOMETRY, and treating them as if
		   they did is what would go wrong if the index test were simply deleted.
		   A DYNLIGHT_SPHERE stores its ORIGIN in positions[0] and its RADIUS in
		   positions[1].x - positions[1] and [2] are not vertices at all - so the
		   triangle centroid and the cross-product area below are meaningless for
		   one, and the cross product of a radius and a direction vector is
		   garbage of an arbitrary magnitude.

		   All three are put in the same currency: a SOLID ANGLE, expressed as
		   `emit_area / dist2`, which is what the polygon path always computed
		   (`area / dist2`) and is therefore bit-identical for the emissive
		   surfaces that were already working. */
		float vol_scale = light.volumetric_scale;

		// resolved on the CPU in copy_light(), so 0 here really does mean "this
		// light lights the room and makes no fog" - skip it before spending a ray
		if (vol_scale <= 0.0)
			continue;

		uint light_type = uint(light.type);

		vec3  lp;                  // the emitter's centre
		float emit_area;           // area whose ratio to dist2 is the solid angle
		float emit_profile = 1.0;  // the emitter's own directional falloff

		if (light_type == DYNLIGHT_POLYGON)
		{
			// emissive triangle; its centroid is close enough for a volumetric
			lp = (light.positions[0] + light.positions[1] + light.positions[2]) / 3.0;
		}
		else
		{
			// sphere and spot both store origin in [0] and radius in [1].x
			lp = light.positions[0];
		}

		vec3 d = lp - p;
		float dist2 = max(dot(d, d), 64.0);   // clamp so standing in a light does not blow up

		if (light_type == DYNLIGHT_POLYGON)
		{
			// AREA MATTERS, and leaving it out is why emissive surfaces did not
			// light the fog. light.color is RADIANCE - per unit area, per
			// steradian - so a big ceiling panel and a tiny one carry the SAME
			// colour and differ only in size. Using colour/dist2 alone therefore
			// made every emissive surface contribute as if it were a pinpoint,
			// which is a huge underestimate, while dlight-derived lights (whose
			// colour already encodes intensity/25, a power-like quantity) drowned
			// them out completely. Power goes as radiance * area, so fold the
			// triangle's area in.
			vec3 cr = cross(light.positions[1] - light.positions[0],
			                light.positions[2] - light.positions[0]);
			float cr_len = length(cr);

			emit_area = 0.5 * cr_len;

			// A SURFACE ONLY EMITS INTO ITS FRONT HEMISPHERE. Without this test a
			// light fixture mounted on a wall lit the fog on BOTH sides of that
			// wall, which - since none of these samples are shadowed - is the main
			// way light was still leaking into places it has no business being. It
			// is the cheapest occlusion there is: one dot product, no ray.
			//
			// Sphere lights get no equivalent because they genuinely do emit in
			// every direction, and a spot light's direction is handled by its
			// emission profile instead.
			if (cr_len > 0)
			{
				// light_polys are wound so the normal faces AWAY from the emitting
				// side, which is why this tests for the negative half-space
				float facing = dot(cr / cr_len, normalize(-d));
				if (facing <= 0)
					continue;
			}
		}
		else
		{
			// The projected disc of the emitter sphere. pi*r^2/dist2 is the
			// small-angle solid angle it subtends, which is the same quantity
			// area/dist2 is for the triangle above - so the two types are
			// directly comparable and one pt_fog_light_scale serves both.
			//
			// The exact form is 2*pi*(1 - sqrt(1 - (r/d)^2)), which this
			// approximates. They agree to within a percent for anything more
			// than a couple of radii away, and closer than that dist2's own
			// 64-unit floor is already the dominant approximation.
			float emit_radius = light.positions[1].x;
			emit_area = M_PI * emit_radius * emit_radius;

			if (light_type == DYNLIGHT_SPOT)
			{
				// cosine of the angle between the spot's axis and the direction
				// out towards this point in the fog
				float cos_theta = dot(normalize(-d), light.positions[2]);

				emit_profile = spotlight_falloff(light.positions,
				                                     light.spot_emission_profile,
				                                     cos_theta);

				// outside the cone entirely - no ray, no further work
				if (emit_profile <= 0.0)
					continue;
			}
		}

		/* IS THE LIGHT ACTUALLY VISIBLE FROM THIS POINT?

		   Until this existed, nothing in this function tested occlusion. The
		   comment at the top of the function says so plainly - "These samples
		   are unshadowed" - and the PVS/cluster restriction was the only thing
		   keeping it in check. But PVS is per-CLUSTER: inside the camera's own
		   cluster a fixture lights fog straight through walls, floors and roofs.

		   That is why fog appears above the top of a base structure with no light
		   anywhere near it, and it is the reason a glow could never be made to
		   pool: pt_fog_light_falloff shapes the light with DISTANCE, and the
		   offending fixture is genuinely close - it is just on the other side of
		   the roof. No distance curve can express "and not through that".

		   Placed AFTER the cheap rejections (model light, back-facing) on
		   purpose, so a ray is only ever spent on a light that would otherwise
		   contribute. t_max stops short of the emitter so the ray cannot hit the
		   emissive surface it is aiming at.

		   COST. One ray per sampled light per evaluation point. In the FROXEL
		   GRID that is ~901k cells x pt_fog_light_samples, and the temporal
		   history smooths what is left, which is what the grid is for. In the
		   per-pixel MARCH it is that many rays per step of every half-res pixel -
		   tens of millions - so expect it to be very expensive there. Default
		   off; turn it on with pt_fog_froxel 1. */
		if (global_ubo.pt_fog_light_shadow != 0)
		{
			float sdist = sqrt(dist2);
			if (sdist > 2.0)
			{
				rayQueryEXT shadow_rq;
				rayQueryInitializeEXT(shadow_rq, FOG_TLAS,
					gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
					AS_FLAG_OPAQUE,
					p, 1.0, d / sdist, max(sdist - 4.0, 1.0));

				rayQueryProceedEXT(shadow_rq);

				if (rayQueryGetIntersectionTypeEXT(shadow_rq, true)
				    != gl_RayQueryCommittedIntersectionNoneEXT)
					continue;
			}
		}

		vec3 color = light.color;

		/* HOW FAST A LIGHT'S FOG GLOW FALLS OFF WITH DISTANCE.

		   This was a bare 1/dist2 - physically correct for a point emitter, and
		   the reason the fog reads as a uniform wash rather than as a pool of
		   glow around each fixture. Inverse square never reaches zero, and once
		   the triangle AREA is folded in (a ceiling panel is hundreds of square
		   units) a light on the far side of a room still contributes real
		   brightness. Nothing here is shadowed either, so there is no occlusion
		   to cut it off. The net effect is that every light in the cluster lifts
		   the whole room's fog by roughly the same amount.

		   pt_fog_light_knee does NOT help with this. It is a compressor on the
		   SUMMED total (below), so it dims the wash and the glow together and
		   cannot change the spatial shape at all - which is why reaching for it
		   as a "falloff" control appears to do nothing.

		   Two knobs, both no-ops at their defaults:

		   pt_fog_light_falloff  the distance exponent. 2 = inverse square, i.e.
		                         exactly what this always did. 3 or 4 pulls the
		                         glow in tight around the source.
		   pt_fog_light_pivot    the distance at which brightness does NOT move
		                         as the exponent changes. Without it, raising the
		                         exponent dims everything at every distance and
		                         pt_fog_light_scale has to be re-tuned to
		                         compensate; with it the exponent is a pure
		                         reshape - brighter than before inside the pivot,
		                         dimmer outside, unchanged at it.
		   pt_fog_light_radius   hard cutoff in world units, 0 = off. Uses the
		                         windowed inverse square (1 - (d/r)^4)^2, which
		                         leaves the near field alone and eases to exactly
		                         zero at r instead of clipping - a hard clip puts
		                         a visible edge in the fog where the window ends.

		   dist2 is already floored at 64 above, so a high exponent cannot blow
		   up when the camera stands inside a fixture. */
		/* THE EXPONENT ONLY EVER STEEPENS THE FAR FIELD. It must NEVER brighten
		   the near field, and the first version of this did exactly that.

		   That version normalised the whole curve about the pivot:
		     atten = pow(pivot2/dist2, falloff*0.5) / pivot2
		   which is unchanged AT the pivot but grows without bound inside it.
		   dist2 is floored at 64 (8 units) and the pivot defaults to 128, so at
		   the floor pivot2/dist2 = 256 and the near field went up by 256^((f-2)/2):
		   256x at falloff 4, 4 billion at falloff 10. Raising the knob therefore
		   made the fog BRIGHTER and blobbier - Matt: "when you set it to 10 it
		   starts to look more foggy" - and by 50 the term overflowed to Inf,
		   which is what the "little black squares hovering around the light"
		   were: NaN cells in the volume.

		   The fix is to multiply the physical inverse square by a factor that is
		   exactly 1 inside the pivot and only ever shrinks outside it. The near
		   field then keeps its correct brightness at every setting, and the knob
		   does one thing only: decide how fast the glow dies with distance.
		   That is what "fall off with the brightness of the light" asks for. */
		float fog_falloff = clamp(global_ubo.pt_fog_light_falloff, 0.0, 64.0);

		float atten = 1.0 / dist2;

		if (abs(fog_falloff - 2.0) > 1e-4)
		{
			float pivot2 = max(global_ubo.pt_fog_light_pivot
			                 * global_ubo.pt_fog_light_pivot, 1.0);

			// min(...) pins this to 1 within the pivot, so the extra decay only
			// ever applies beyond it. At falloff 2 the exponent is 0 and the
			// factor is 1, i.e. identical to plain inverse square.
			atten *= pow(min(pivot2 / dist2, 1.0), (fog_falloff - 2.0) * 0.5);
		}

		float fog_radius = global_ubo.pt_fog_light_radius;
		if (fog_radius > 0.0)
		{
			float x = dist2 / (fog_radius * fog_radius);
			float w = clamp(1.0 - x * x, 0.0, 1.0);
			atten *= w * w;
		}

		// emit_area/dist2 is the solid angle, whatever the light's type;
		// emit_profile is 1 for everything but a spot light; light_style_scale is
		// what makes a strobing emissive strobe in the fog too; vol_scale is the
		// per-light volumetric scale, already resolved against its class default
		// on the CPU.
		sum += color * emit_area * emit_profile
		     * light.light_style_scale * vol_scale * atten;
	}

	/* Scale back up for the lights this step did not look at.

	   THIS IS UNBIASED BUT HIGH VARIANCE, AND A STEEP FALLOFF MAKES IT WORSE -
	   which is the mottled, blotchy look in the fog, and why turning
	   pt_fog_light_falloff up can make the fog noisier rather than cleaner.

	   A steep falloff means almost all the energy sits in the handful of lights
	   nearest the point, so whether the strided subset happens to catch one
	   swings the answer by the full rescale factor.

	   The other half of this used to be that `count` was the WHOLE cluster list
	   while the loop skipped every model light in it (base1 carries 37), so many
	   samples contributed nothing yet the survivors were still multiplied by
	   count/taken. That is GONE - every light in the list is now evaluated - so
	   the estimator wastes a sample only on a light whose volumetric_scale is 0
	   or which is genuinely occluded.

	   The estimator is still correct in expectation - it is the per-step
	   variance that shows. pt_fog_light_samples buys it down directly; the
	   froxel grid's temporal history (pt_fog_froxel 1) averages it across frames
	   instead, which is the cheaper answer and is what that grid is for. */
	vec3 result = sum * (float(count) / float(taken));

	// SOFT KNEE on the local-light total.
	//
	// This sum is linear in how many lights a cluster holds, and the maps differ
	// by orders of magnitude: base1 is an interior packed with emissive surfaces,
	// mgu5m1 has a handful. Linear, no single brightness scale can satisfy both -
	// base1 came out "way too foggy" at exactly the setting that left mgu5m1
	// wanting more. x/(1+x/knee) is linear well below the knee and rolls off
	// above it, so a sparse map is left alone while a light-dense one is
	// compressed. Physically it is a fudge; perceptually it is what the eye
	// expects, and it is the same shape a tone curve applies.
	/* THE KNEE IS WHAT STOPS THE GLOW EVER LOOKING TIGHT. Read this before
	   reaching for pt_fog_light_falloff again.

	   x/(1+x/knee) SATURATES AT knee. At the 2.0 default, a march point sitting
	   in a bright fixture with a raw sum of 1000 comes out at 1.996, while a dim
	   corner with a raw sum of 0.5 comes out at 0.4. A physical contrast of
	   2000:1 is delivered as 5:1. The compressor squashes precisely the range
	   that makes a light read as a glowing cloud with darkness around it, and
	   leaves the faint room-wide wash almost untouched - so no amount of
	   distance shaping can produce a tight pool while it is on hard.

	   It was added for a real reason: base1 is packed with emissive surfaces and
	   went "way too foggy" at the setting mgu5m1 needed. But that is a
	   per-map BRIGHTNESS problem, and pt_fog_light_scale is the knob for it.

	   0 disables the compressor entirely - the honest linear answer, and the
	   right starting point for anyone tuning for localised glow. */
	float knee = global_ubo.pt_fog_light_knee;
	if (knee > 0.0)
		result = result / (1.0 + result / knee);

	// The sky is added OUTSIDE the knee - the knee exists to compress a room
	// crowded with emissive surfaces, and the sky is a single ambient term that
	// has nothing to do with how many of those there are. Computed at the top of
	// this function so the early returns above cannot skip it.
	return result + sky_term;
}

/*
=================
A SMALL RNG FOR THE RIS CANDIDATE LOOP.

The blue noise the callers hand in is only three values, and resampled
importance sampling below needs two per candidate.  Blue noise is worth having
for the sample placed ON the chosen light - that one lands in the image - but
for deciding which candidates to look at, any decorrelated sequence does, and an
LCG costs no texture fetches.
=================
*/
uint fog_rng_state;

// fog_debug_sky_term is declared up beside getSkyVisibility now, so that the
// mode-2 path writes it as well - see the note there.

float fog_rand()
{
	fog_rng_state = fog_rng_state * 1664525u + 1013904223u;
	return float(fog_rng_state >> 8u) * (1.0 / 16777216.0);
}

/*
The medium's directionality. Needed BEFORE the candidate loop because the RIS
target function uses it, and needed identically in every pass that evaluates
that target - the reservoir, spatial and scatter passes all call this rather
than each rolling their own, because a g that differs between them would make
the MIS weights answer about a different integrand than the one being sampled.

CLAMPED, and the clamp is not cosmetic. ScatterPhase_HenyeyGreenstein computes
num/denom with num = 1 - abs(g) and denom = sqrt(1 - 2*g*cosa + g*g); at g = 1
looking straight at the light both are ZERO and the phase is 0/0 = NaN. A NaN
reaches the froxel volume, the temporal history blends it with itself forever,
and the tone mapper adapts to a frame containing NaN by going black - which does
not clear when the cvar is put back, only on a restart. god_rays.comp already
clamped its own copy to 0.95 for the sun term; this one did not.

Negative g (back-scattering) is legal and useful, so the clamp is two-sided
rather than a max().
*/
float fog_eccentricity()
{
	float g = global_ubo.god_rays_eccentricity;
	if (global_ubo.pt_fog_eccentricity >= 0.0)
		g = global_ubo.pt_fog_eccentricity;
	return clamp(g, -0.95, 0.95);
}

/*
=================
volume_light_estimate

A RAY-FREE guess at how much a light will contribute at p, used only to decide
which light is worth spending the one visibility ray on.  It does not have to be
accurate - RIS divides it back out - but the closer it tracks the real
contribution the lower the variance, and ANY LARGE FACTOR LEFT OUT OF IT SHOWS UP
AS NOISE THAT MORE CANDIDATES CANNOT REMOVE.

THE PHASE FUNCTION IS IN HERE FOR EXACTLY THAT REASON, and leaving it out was a
real bug rather than an approximation.  gr_eccentricity defaults to 0.75 and
pt_fog_eccentricity -1 follows it, and Henyey-Greenstein at g = 0.75 returns
0.597 for a light straight ahead against 0.00174 for one straight behind - a
factor of 343.  With that missing, RIS ranked a light the viewer is facing away
from identically to one they are looking at, picked it just as often, and then
the real evaluation multiplied it by ~1/343.  The correction W is what makes
this unbiased, so the picture was right on average and simply very noisy - and
raising pt_fog_light_samples did nothing at all, because more candidates drawn
against the wrong target is still the wrong target.  That is precisely the
symptom that was reported.

The emitter's own cosine is folded in for the same reason; it was previously
only a binary front/back test.

What remains outside it is VISIBILITY, which cannot be known without the ray
this function exists to place.  That is the noise the spatial filter and the
temporal history are left to deal with.
=================
*/
float volume_light_estimate(LightPolygon light, vec3 p, vec3 view_dir, float g)
{
	// sky brushes are marked with a negative colour and belong to sky_term
	if (any(lessThan(light.color, vec3(0.0))))
		return 0.0;

	float vol_scale = light.volumetric_scale;
	if (vol_scale <= 0.0)
		return 0.0;

	uint light_type = uint(light.type);

	vec3  lp;
	float emit_area;
	float emit_profile = 1.0;

	if (light_type == DYNLIGHT_POLYGON)
	{
		lp = (light.positions[0] + light.positions[1] + light.positions[2]) / 3.0;

		vec3 cr = cross(light.positions[1] - light.positions[0],
		                light.positions[2] - light.positions[0]);
		float cr_len = length(cr);
		emit_area = 0.5 * cr_len;

		if (cr_len > 0.0)
		{
			/* THE DIRECTION HERE MUST BE p -> LIGHT, AND IT USED TO BE THE
			   OPPOSITE. That one sign made EVERY emissive surface in the map
			   contribute exactly ZERO fog, at any volumetric_scale,
			   default_radiance, emissive_factor or pt_fog_scale_emissive.

			   The convention is light_lists.h:281, which is what the path tracer
			   itself shades surfaces with:

			       vec3 L = normalize(position_light - p);      // p -> light
			       float LdotNL = max(0, -dot(light_normal, L));

			   getVolumeLightInscatter already matched that exactly. This function
			   did not: it used normalize(p - lp), which is LIGHT -> p, so
			   cos_emit came out as the exact NEGATION of the real evaluation's
			   profile.

			   The two therefore accept DISJOINT sets of triangles. However a
			   light poly is wound, one of the two rejects it: either the estimate
			   returns 0 and the light never becomes a RIS candidate at all, or it
			   is picked and then getVolumeLightInscatter hits `profile <= 0` and
			   returns sky_term. Emissive surfaces are DYNLIGHT_POLYGON and are ~95%
			   of a map's light, so this silently removed almost all of it from the
			   volumetrics while leaving surface lighting untouched - and the fog
			   still looked alive because sphere and spot lights have no cosine
			   test in this branch, so every `light place` light and every muzzle
			   flash still worked. That is exactly the reported asymmetry.

			   sqrt() to match the softening the real evaluation applies. */
			float cos_emit = -dot(cr / cr_len, normalize(lp - p));
			if (cos_emit <= 0.0)
				return 0.0;
			emit_profile *= sqrt(cos_emit);
		}
	}
	else
	{
		// sphere and spot: positions[0] is the ORIGIN, positions[1].x the RADIUS
		lp = light.positions[0];
		float emit_radius = light.positions[1].x;
		emit_area = M_PI * emit_radius * emit_radius;

		if (light_type == DYNLIGHT_SPOT)
		{
			emit_profile = spotlight_falloff(light.positions, light.spot_emission_profile,
			                                 dot(normalize(p - lp), light.positions[2]));
			if (emit_profile <= 0.0)
				return 0.0;
		}
	}

	vec3 d = lp - p;
	float dist2 = max(dot(d, d), 64.0);

	// The phase function about THIS light's direction - the factor whose absence
	// made more candidates pointless. See the note above.
	float phase = ScatterPhase_HenyeyGreenstein(dot(view_dir, normalize(d)), g);

	return luminance(light.color) * emit_area * emit_profile * phase
	     * vol_scale * light.light_style_scale / dist2;
}

/*
=================
getVolumeLightInscatter  -  cl_fog 3

In-scatter at a point, evaluated the way the path tracer evaluates a surface.

WHY THIS EXISTS ALONGSIDE getClusterLightInscatter, RATHER THAN REPLACING IT.
That function computes light by hand: emitter centroid, triangle area, a bare
1/dist2, then pt_fog_light_falloff / _pivot / _radius to reshape the curve and
pt_fog_light_knee to compress the total. Every one of those knobs exists to
patch a consequence of not doing the integral properly, and together they make
the fog impossible to tune. It is kept untouched so cl_fog 2 renders exactly as
it always has.

This one draws an actual point ON one light and divides by that sample's
solid-angle pdf, which is what light_lists.h has always done for surfaces:

    contribution = radiance * (1 / pdfw) * emitter_profile * phase * visibility

1/pdfw carries the emitter's true solid angle from p. Inverse-square falls out
of it for free, and correctly for all three light types. So there is no falloff
exponent to set, no pivot, no cutoff radius, and because the values are bounded
by real occlusion instead of accumulating without limit, no knee. cl_fog 3
ignores all five.

RESAMPLED IMPORTANCE SAMPLING, and why the first version without it looked like
coloured blobs. That version took a STRIDED SUBSET of the cluster's lights,
evaluated all of them, and multiplied by count/taken to make up for the ones it
skipped. On a lava map the cluster holds hundreds of lights, so that rescale is
a factor of ~25 applied to whichever handful the stride happened to land on -
and neighbouring froxels landing on different handfuls differ by the whole
factor. In the froxel grid one sample is shared by every pixel in the cell, so
the disagreement is not fine noise that a filter removes, it is a solid block of
the wrong brightness. That is the blotchiness, and no amount of temporal
accumulation fixes it because the error is correlated across the cell.

RIS replaces the uniform subset with a weighted draw: look at M candidates
cheaply (volume_light_estimate, no rays), keep ONE with probability proportional
to its estimated contribution, and correct with the reservoir weight. The
survivor is nearly always a light that actually matters, so the variance
collapses - and only ONE visibility ray is traced per point instead of one per
candidate, which makes this CHEAPER than the version it replaces as well as
quieter. This is what RTX Remix's initialRISSampleCount does.

THE VISIBILITY RAY IS NOT OPTIONAL HERE, and deliberately does not honour
pt_fog_light_shadow. Occlusion is what makes light pool in a volume instead of
washing it; without it no amount of tuning produces the look this mode is for.
The cvar defaults to 0 in several shipped map cfgs (mgu5m1 among them), so
respecting it would silently cripple this mode on exactly the maps most likely
to be used to judge it.

`rnd` is three blue-noise values; yz place the sample on the chosen light.
`seed` seeds the candidate loop's own RNG.
=================
*/
/* THE SKY HALF, ON ITS OWN.

   Split out of getVolumeLightInscatter so the ReSTIR passes can compose the two
   halves in a different order: the reservoir and spatial passes decide WHICH
   light without tracing anything at all, and the scatter pass then adds this to
   whatever they chose. Still one sky ray per cell either way - the split
   duplicates no work, it only moves where the pieces are called from. */
vec3 fog_sky_inscatter(vec3 sky_p)
{
	/* UNDER THE PHYSICAL SKY, THE SUN TERM IS THE SKY FOG - exactly as cl_fog 1
	   does it, which is what Matt asked for.

	   The two skies are not the same kind of thing. A map SKYBOX is a picture:
	   it has no sun, `sun_light->visible` is false, and the god-rays sun term
	   contributes nothing - so mode 3's ambient sky term below is the only sky
	   light the fog can get, and it is what makes mode 3 look right there. The
	   PHYSICAL sky is a rendered atmosphere WITH a real sun, so the sun term in
	   god_rays.comp is already producing shafts through openings, shadowed by
	   the shadow map and shaped by pt_fog_eccentricity. Adding an unshadowed,
	   isotropic ambient term on top of those does not add sky light so much as
	   wash the shafts out - and the ambient half has no directionality to lose,
	   so it survives as a flat slab while the shafts drown.

	   So: physical sky -> the sun term alone, i.e. cl_fog 1's sky fog. Map
	   skybox -> unchanged, because mode 3 already works there.

	   The sun term is NOT gated by fog_mode - it runs for every mode in
	   god_rays.comp - so nothing has to be turned on for this to leave the sky
	   lit. This only STOPS the second contribution.

	   NOTE this function is reached only from mode 3 (getVolumeLightInscatter
	   and the ReSTIR scatter path). Mode 2 has its own inline sky block in
	   getClusterLightInscatter and is deliberately untouched: it stays the
	   unchanged A/B reference it was built to be. */
	if (fog_sun_is_the_sky())
	{
		fog_debug_sky_term = vec3(0);
		return vec3(0);
	}

	// The sky is ambient and has no preferred direction, so it keeps the
	// isotropic phase rather than the medium's. Computed first for the same
	// reason as in getClusterLightInscatter: an open area with no emissive
	// surfaces in its cluster must still see the sky.
	vec3 sky_term = vec3(0);
	float sky_vis = getSkyVisibility(sky_p);
	if (sky_vis > 0.0)
	{
		vec3 sky_radiance = vec3(0);

		if (global_ubo.environment_type == ENVIRONMENT_DYNAMIC)
			sky_radiance = sun_color_ubo.sky_color;
		else if (global_ubo.environment_type == ENVIRONMENT_STATIC)
			sky_radiance = vec3(global_ubo.fog_sky_r,
			                    global_ubo.fog_sky_g,
			                    global_ubo.fog_sky_b);

		sky_term = sky_radiance
		         * FOG_SKY_SOLID_ANGLE * global_ubo.pt_fog_sky_scale
		         * sky_vis * FOG_ISOTROPIC_PHASE;
	}

	fog_debug_sky_term = sky_term;

	/* pt_fog_isolate splits the two halves of the fog's lighting so each can be
	   watched on its own, fully composited. Zeroing the sky HERE rather than at
	   the end matters for 2: the sky term is ADDED to the light total by every
	   caller, so a caller cannot subtract it back out.

	   Isolate 1 - "sky only" - is NOT handled here, because it is a statement
	   about the LIGHT half (skip it, and the rays it costs) and this function no
	   longer owns that half. Each caller applies it. */
	if (int(global_ubo.pt_fog_isolate) == 2)
	{
		sky_term = vec3(0);
		fog_debug_sky_term = sky_term;
	}

	return sky_term;
}

/*
=================
THE RIS RESERVOIR

Everything about "which of the cluster's lights is worth the one visibility ray"
in a form that can be STORED and shared between froxels, which is what spatial
reservoir reuse needs.

  y      the chosen light's index, ~0u when the reservoir holds no sample
  wsum   sum of the candidates' weights, w_i = p_hat_i / pdf_i
  M      how many candidate DRAWS this reservoir represents - including draws
         that scored zero, because the estimator's denominator counts attempts,
         not successes
  p_hat  the target function of y evaluated at the point that built this
         reservoir, kept so a neighbour can reconstruct wsum = p_hat * W * M
         without re-deriving it

THE SAMPLE SPACE IS DISCRETE, AND THAT IS WHY THIS IS CHEAP TO SHARE. A
reservoir holds a light INDEX, not a point on that light - the point is drawn
afterwards, per cell, from that cell's own blue noise. Surface ReSTIR has to
carry a Jacobian when a neighbour's sample is reinterpreted from a different
shading point, because the sample lives in a continuous solid-angle domain that
changes shape. An index does not: reusing "light 47" at a neighbouring froxel is
exact, with no reparameterization and therefore no Jacobian at all.
=================
*/
struct FogReservoir
{
	uint  y;
	float wsum;
	float M;
	float p_hat;
};

FogReservoir fog_reservoir_empty()
{
	FogReservoir r;
	r.y     = ~0u;
	r.wsum  = 0.0;
	r.M     = 0.0;
	r.p_hat = 0.0;
	return r;
}

/* The RIS weight: what the chosen sample's true contribution must be multiplied
   by for the estimator to be unbiased. Zero for a reservoir that never found a
   non-zero candidate, which is a legitimate outcome and not an error. */
float fog_reservoir_W(FogReservoir r)
{
	return (r.M > 0.0 && r.p_hat > 0.0) ? (r.wsum / (r.M * r.p_hat)) : 0.0;
}

/* The target function of a given light at an ARBITRARY point.

   Spatial reuse needs exactly this and the existing code did not expose it: the
   MIS bias correction has to ask "what would light y have been worth at my
   neighbour's own sample point", which means evaluating the target somewhere
   other than where the reservoir was built. */
float fog_target(uint light_idx, vec3 p, vec3 view_dir, float g)
{
	if (light_idx == ~0u)
		return 0.0;

	return volume_light_estimate(get_light_polygon(light_idx), p, view_dir, g);
}

/*
=================
fog_ris_initial

The candidate loop, unchanged in behaviour and now callable on its own so a pass
with no rays in it can do this work.

ONE DIFFERENCE FROM THE CODE THIS REPLACES, AND IT IS NUMERICALLY INERT: the
candidates' source pdf (1/count, uniform over the cluster list) is divided out
HERE, inside wsum, instead of being multiplied back in at the W line. Both give
W = wsum*count/(M*p_hat) exactly; the reservoir's selection test is a ratio of
two weights and is untouched by scaling both. The point of moving it is that
wsum then means the same thing in every reservoir, which is what makes two of
them addable - a neighbour has no way to know the count its source cell used.
=================
*/
FogReservoir fog_ris_initial(uint cluster_idx, vec3 p, vec3 view_dir, float g, uint seed)
{
	FogReservoir r = fog_reservoir_empty();

	if (cluster_idx == ~0u)
		return r;

	uint list_start = light_buffer.light_list_offsets[cluster_idx];
	uint list_end   = light_buffer.light_list_offsets[cluster_idx + 1];

	uint count = list_end - list_start;
	if (count == 0)
		return r;

	fog_rng_state = seed;
	fog_rand();   // one step off the seed, so neighbouring seeds do not correlate

	// --- RIS: pick one light out of M candidates, weighted by estimate ---

	uint M = uint(clamp(global_ubo.pt_fog_light_samples, 1.0, 32.0));

	// The DRAW count, set before the loop. A candidate that scores zero still
	// consumed a draw, and leaving it out of the denominator would bias the
	// estimator upwards by exactly the fraction of the list that contributes
	// nothing - which on a big map is most of it.
	r.M = float(M);

	for (uint i = 0; i < M; i++)
	{
		uint n_idx = list_start + (uint(fog_rand() * float(count)) % count);
		uint light_idx = light_buffer.light_list_lights[n_idx];
		if (light_idx == ~0u)
			continue;

		LightPolygon cand = get_light_polygon(light_idx);

		float estimate = volume_light_estimate(cand, p, view_dir, g);
		if (estimate <= 0.0)
			continue;

		/* Candidates are drawn UNIFORMLY from the cluster list, so each has
		   source pdf 1/count and weight w_i = estimate_i * count. The RIS
		   estimator is

		       f(x) / p_hat(x) * (1/M) * sum(w_i)

		   which is f(x) * W with W = wsum / (M * p_hat). p_hat is
		   volume_light_estimate, which need not match f at all - the ratio is
		   what makes this unbiased. */
		float w = estimate * float(count);

		r.wsum += w;

		// keep this one with probability w/wsum - the standard streaming
		// reservoir update, so every candidate ends up held in proportion to its
		// weight after a single pass
		if (fog_rand() * r.wsum <= w)
		{
			r.y     = light_idx;
			r.p_hat = estimate;
		}
	}

	return r;
}

/*
=================
fog_shade_chosen

Everything downstream of "which light": draw a point on it, spend the one
visibility ray, apply the phase function and the firefly clamp.

Split from the candidate loop so that the ReSTIR passes - which choose the light
without tracing anything - can hand their answer straight to it. The light half
only; the sky is fog_sky_inscatter's and the caller adds the two.
=================
*/
vec3 fog_shade_chosen(vec3 p, vec3 view_dir, float g, vec3 rnd, uint chosen_idx, float W)
{
	if (chosen_idx == ~0u || !(W > 0.0))
		return vec3(0);

	LightPolygon light = get_light_polygon(chosen_idx);

	// Draw a point on the emitter and get the pdf of having drawn it, measured
	// in solid angle from p. These are the same functions the path tracer
	// samples surfaces with, and none of them needs a normal at the receiving
	// end - which is exactly why a volume can use them.
	vec3  pos_light;
	vec3  light_normal;
	float pdfw = 0.0;

	switch (uint(light.type))
	{
	case DYNLIGHT_POLYGON:
		pos_light = sample_projected_triangle(p, light.positions, rnd.yz, light_normal, pdfw);
		break;
	case DYNLIGHT_SPHERE:
		pos_light = sample_projected_sphere(p, light.positions, rnd.yz, light_normal, pdfw);
		break;
	case DYNLIGHT_SPOT:
		pos_light = sample_projected_spotlight(p, light.positions, light.spot_emission_profile,
		                                       rnd.yz, light_normal, pdfw);
		break;
	default:
		return vec3(0);
	}

	if (pdfw <= 0.0)
		return vec3(0);

	vec3 d = pos_light - p;
	float dist = length(d);
	if (dist < 1e-4)
		return vec3(0);

	vec3 L = d / dist;

	// The emitter's own cosine. sqrt() of it is light_lists.h's convention for
	// softening a surface's emission profile, kept so a light casts the same
	// relative brightness into the fog as onto a wall.
	float profile = sqrt(max(0.0, -dot(light_normal, L)));
	if (profile <= 0.0)
		return vec3(0);

	// Occlusion. One ray, on the light RIS decided was worth it. See the header
	// note: not gated on pt_fog_light_shadow. t_max stops short of the sampled
	// point so the ray cannot hit the emissive surface it is aiming at.
	{
		rayQueryEXT shadow_rq;
		rayQueryInitializeEXT(shadow_rq, FOG_TLAS,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
			AS_FLAG_OPAQUE,
			p, 0.05, L, max(dist - 1.0, 0.01));

		rayQueryProceedEXT(shadow_rq);

		if (rayQueryGetIntersectionTypeEXT(shadow_rq, true)
		    != gl_RayQueryCommittedIntersectionNoneEXT)
			return vec3(0);
	}

	// The phase function is applied PER LIGHT, about that light's own direction,
	// which is the whole reason a volumetric looks different when you face a
	// lamp than when it is behind you. g was settled before the candidate loop,
	// because the RIS target function needs the same value.
	float phase = ScatterPhase_HenyeyGreenstein(dot(view_dir, L), g);

	vec3 contribution = light.color
	                  * (profile * phase * light.volumetric_scale
	                     * light.light_style_scale * W / pdfw);

	/* FIREFLY CLAMP.

	   RIS removes most of the variance but not the tail: a froxel that happens
	   to sit very close to a small emitter gets a tiny pdfw and therefore an
	   enormous contribution, and in the grid that one sample is shared by every
	   pixel of the cell. Clamping the sample's luminance - rather than dropping
	   it - keeps the light present but stops one cell blowing out.

	   This is biased on purpose, and it is what RTX Remix's
	   froxelFireflyFilteringLuminanceThreshold does. 0 disables it. */
	float firefly = global_ubo.pt_fog_firefly;
	if (firefly > 0.0)
	{
		float lum = luminance(contribution);
		if (lum > firefly)
			contribution *= firefly / lum;
	}

	return contribution;
}

/*
=================
getVolumeLightInscatter  -  the whole of cl_fog 3 for one point.

A composition of the three pieces above rather than one body, so that the froxel
passes can take the same pieces apart and put a spatial reuse step in the
middle. Unchanged in behaviour and unchanged in cost: one sky ray, one candidate
loop, one visibility ray, in that order.

This is still what the per-pixel march (pt_fog_froxel 0) calls, and what the
grid calls when pt_fog_restir is off.
=================
*/
vec3 getVolumeLightInscatter(uint cluster_idx, vec3 p, vec3 sky_p, vec3 view_dir, vec3 rnd, uint seed)
{
	vec3 sky_term = fog_sky_inscatter(sky_p);

	// "sky only" - skips the candidate loop and the visibility ray with it
	if (int(global_ubo.pt_fog_isolate) == 1)
		return sky_term;

	float g = fog_eccentricity();

	FogReservoir r = fog_ris_initial(cluster_idx, p, view_dir, g, seed);

	return fog_shade_chosen(p, view_dir, g, rnd, r.y, fog_reservoir_W(r)) + sky_term;
}

// The scattering albedo at a point: the map's height-fog gradient, lerped by
// altitude between end_color at the bottom of the band and start_color at the
// top. Maps that define only the plain distance fog use fog_color instead.
vec3 getFogColor(vec3 p)
{
	if (global_ubo.fog_enable == 0)
		return vec3(1.0);

	if (global_ubo.fog_hf_density <= 0)
		return vec3(global_ubo.fog_color_r, global_ubo.fog_color_g, global_ubo.fog_color_b);

	float band = global_ubo.fog_hf_start_z - global_ubo.fog_hf_end_z;
	float t = (band > 0) ? clamp((p.z - global_ubo.fog_hf_end_z) / band, 0.0, 1.0) : 0.0;

	vec3 bottom = vec3(global_ubo.fog_hf_end_r,   global_ubo.fog_hf_end_g,   global_ubo.fog_hf_end_b);
	vec3 top    = vec3(global_ubo.fog_hf_start_r, global_ubo.fog_hf_start_g, global_ubo.fog_hf_start_b);
	vec3 color  = mix(bottom, top, t);

	// A few maps leave one of the two colours black. Taken literally that kills
	// all scattered light in half the band and reads as a bug, so fall back to
	// the other end rather than to nothing.
	if (dot(color, vec3(1.0)) < 0.01)
		color = (dot(top, vec3(1.0)) > dot(bottom, vec3(1.0))) ? top : bottom;

	return color;
}

float getDensity(vec3 p)
{
	// function that is 1.0 within the world box, and trails off to 0 by 25% of world size outside the box
	vec3 bounds = clamp(3.0 - 2.0 * abs((p - global_ubo.world_center.xyz) * global_ubo.world_half_size_inv.xyz), vec3(0), vec3(1));
	float world_box = bounds.x * bounds.y * bounds.z;

	// Without a map fog definition this is the original flat medium, so god rays
	// on the classic campaign are completely unchanged.
	if (global_ubo.fog_enable == 0)
		return world_box;

	float density = getHeightFogDensity(p)
	              + global_ubo.fog_density / FOG_DISTANCEFOG_REFERENCE;

	// The ceiling is pt_fog_density_max. This was a hard-coded 4.0, and 4.0 turned
	// out to be a wall you hit while the fog still looked too thin on the ground.
	//
	// Why the ground is the hard case: in-scatter accumulates as
	// stepLength * density along the ray, and the march runs from the camera to
	// the SURFACE. A ray into the sky crosses the whole world box - thousands of
	// units - while a ray to the floor in front of you is a few dozen. The sky
	// therefore fogs up one to two orders of magnitude sooner than the ground,
	// and density is the only lever that closes that gap.
	//
	// The wall arrives sooner than cl_fog_scale suggests, because cl_fog_scale
	// multiplies the map's authored density BEFORE the REFERENCE normalisation
	// above. A map with only height fog saturates once
	//
	//     heightfog_density * cl_fog_scale / FOG_HEIGHTFOG_REFERENCE >= ceiling
	//
	// so mgu1m1 (heightfog_density 0.00025) hit the old 4.0 at the band floor by
	// cl_fog_scale 400, and at EYE LEVEL - two thirds up its band, where the
	// player actually stands - by about 1075. Past that, turning cl_fog_scale up
	// did nothing at all.
	//
	// The new default is 64.0. That is safe for every shipped map because none of
	// them comes close at the calibrated cl_fog_scale 1.0: the densest is mgu6m3
	// at 0.412, and the next is q2dm4 at 0.134, so there is more than 150x of
	// headroom before this clamp can alter a map's intended look. It only matters
	// once cl_fog_scale is pushed well above 1.
	//
	// Raising it is cheap in time: getStep already bottoms out at its 1-unit
	// minimum by density 1.0, so a higher ceiling costs no extra march steps. It
	// is NOT free in brightness - in-scatter is linear in density with no
	// meaningful extinction in open air, so this scales the fog glow directly.
	return world_box * clamp(density, 0.0, max(0.0, global_ubo.pt_fog_density_max));
}

float getStep(float t, float density)
{
	return max(1, mix(20, 5, density));
}

#endif // FOG_MEDIUM_GLSL_
