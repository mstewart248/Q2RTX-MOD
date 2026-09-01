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
		return global_ubo.fog_sky_fade;   // fall back to the CPU cluster estimate

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
		return 1.0;

	if (soften <= 0.0)
		return 0.0;

	return clamp(rayQueryGetIntersectionTEXT(rq, true) / soften, 0.0, 1.0);
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
// level. These samples are unshadowed - a shadow ray per light per march step is
// not affordable - so without the PVS restriction a single flare lit fog through
// every wall in the map.
//
// The cluster is the one at the surface the ray ends on, not at each march
// point. That is an approximation, but it is the right region's light set and it
// costs one texture fetch instead of a per-step BSP walk.
vec3 getClusterLightInscatter(uint cluster_idx, vec3 p, float rand01)
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
	float sky_vis = getSkyVisibility(p);
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

		// STATIC LIGHTS ONLY - emissive surfaces and the sky.
		//
		// Model lights are packed after the static ones, so this index test
		// drops them. They are the map's invisible dynamic_light point lights
		// (plus muzzle flashes and thrown flares), and they were the one thing
		// still washing the whole level: a point light has no real extent, so
		// the area term cannot damp it the way it damps a surface, and nothing
		// here is shadowed, so a single bright one lit fog through the entire
		// map. An emissive surface behaves because it has genuine geometry and
		// area. base1 carries 37 dynamic_lights, which is why it was the worst
		// case.
		if (light_idx >= uint(global_ubo.num_static_lights))
			continue;

		LightPolygon light = get_light_polygon(light_idx);

		// emissive triangle; its centroid is close enough for a volumetric
		vec3 lp = (light.positions[0] + light.positions[1] + light.positions[2]) / 3.0;

		vec3 d = lp - p;
		float dist2 = max(dot(d, d), 64.0);   // clamp so standing in a light does not blow up

		// A SURFACE ONLY EMITS INTO ITS FRONT HEMISPHERE. Without this test a
		// light fixture mounted on a wall lit the fog on BOTH sides of that
		// wall, which - since none of these samples are shadowed - is the main
		// way light was still leaking into places it has no business being. It
		// is the cheapest occlusion there is: one dot product, no ray.
		vec3 cr_n = cross(light.positions[1] - light.positions[0],
		                  light.positions[2] - light.positions[0]);
		float cr_len = length(cr_n);
		if (cr_len > 0)
		{
			// light_polys are wound so the normal faces AWAY from the emitting
			// side, which is why this tests for the negative half-space
			float facing = dot(cr_n / cr_len, normalize(-d));
			if (facing <= 0)
				continue;
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

		// AREA MATTERS, and leaving it out is why emissive surfaces did not light
		// the fog. light.color is RADIANCE - per unit area, per steradian - so a
		// big ceiling panel and a tiny one carry the SAME colour and differ only
		// in size. Using colour/dist2 alone therefore made every emissive surface
		// contribute as if it were a pinpoint, which is a huge underestimate,
		// while dlight-derived lights (whose colour already encodes
		// intensity/25, a power-like quantity) drowned them out completely.
		// Power goes as radiance * area, so fold the triangle's area in.
		vec3 cr = cross(light.positions[1] - light.positions[0],
		                light.positions[2] - light.positions[0]);
		float area = 0.5 * length(cr);

		/* SKY POLYGONS ARE NOT AREA LIGHTS HERE - THIS FUNCTION ALREADY HAS A SKY
		   TERM, AND COUNTING THEM TWICE IS THE "FOG IN THE SKY" BUG.

		   copy_light() MARKS a sky polygon by storing its colour NEGATIVE
		   (`-sky_radiance * 0.5`, vertex_buffer.c). The old line here was
		   `vec3 color = abs(light.color);` - which does not honour the marker,
		   it ERASES it, turning every sky brush face in the cluster into a
		   perfectly ordinary bright area light.

		   That is ruinous for fog specifically, because `area` is folded into
		   the sum below and a sky brush face is enormous - thousands of square
		   units against a light fixture's tens. So the sky brushes dominated the
		   local-light sum near the top of a map, which is exactly where they
		   are, and produced glowing fog above the rooftops with no fixture
		   anywhere near it.

		   Every symptom follows from this and none of it responded to the knobs:
		     - pt_fog_sky_scale did nothing, because that scales sky_term, and
		       this was arriving through the LOCAL LIGHT path instead;
		     - pt_fog_light_scale killed it, because that scales this sum;
		     - pt_fog_light_shadow did not help, because a sky brush genuinely IS
		       visible from open air - the occlusion test was answering honestly;
		     - setting the physical sky to NIGHT did not dim it, because
		       sky_radiance comes from avg_envmap_color, which is fixed at map
		       load from the skybox images and knows nothing about time of day.

		   The sky's contribution to fog belongs to sky_term at the top of this
		   function, which has its own visibility trace and its own scale. Skip
		   the marked polygons here and it is accounted for exactly once. */
		if (any(lessThan(light.color, vec3(0.0))))
			continue;

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

		sum += color * area * light.light_style_scale * atten;
	}

	/* Scale back up for the lights this step did not look at.

	   THIS IS UNBIASED BUT HIGH VARIANCE, AND A STEEP FALLOFF MAKES IT WORSE -
	   which is the mottled, blotchy look in the fog, and why turning
	   pt_fog_light_falloff up can make the fog noisier rather than cleaner.

	   Two things stack up. `count` is the WHOLE cluster list, including the
	   model lights the loop skips (base1 carries 37 of them), so many of the
	   samples contribute nothing yet the survivors are still multiplied by
	   count/taken. And a steep falloff means almost all the energy sits in the
	   handful of lights nearest the point, so whether the strided subset happens
	   to catch one swings the answer by the full rescale factor.

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
