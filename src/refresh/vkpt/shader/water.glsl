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

vec3 get_water_normal(uint material_id, vec3 geo_normal, vec3 tangent, vec3 position, bool local_space)
{
	// Add flow
	if((material_id & MATERIAL_FLAG_FLOWING) != 0)
	{
		position -= tangent * global_ubo.time * 32;
	}	

	// Remove the sign from the normal to make water simulation uniform,
	// regardless of which side we're looking at the surface from.
	// This is necessary to have caustics motion match the waves.
	vec3 unsigned_geo_normal = abs(geo_normal);

	// Construct a basis around the normal, get object-local 2D position.
	mat3 basis = construct_ONB_frisvad(unsigned_geo_normal);
	vec2 p = vec2(dot(position, basis[0]), dot(position, basis[2]));
	
	// Sample the texture and add a few instances of noise.

	const float speed = 2.5;

	vec2 uv1 = p.xy * 0.006 + global_ubo.time * vec2(0.01, 0.02) * speed;
	vec3 a = global_textureLod(global_ubo.water_normal_texture, uv1, 0).xyz;
	a.xy = a.xy * 2 - vec2(1);
	a.xy *= 0.3;

	vec2 uv2 = p.xy * 0.003 + global_ubo.time * vec2(0.013, 0.014) * speed;
	vec3 b = global_textureLod(global_ubo.water_normal_texture, uv2, 0).xyz;
	b.xy = b.xy * 2 - vec2(1);
	b.xy *= 0.5;

	vec2 uv3 = p.xy * 0.0061 + global_ubo.time * vec2(-0.01, -0.02) * speed;
	vec3 c = global_textureLod(global_ubo.water_normal_texture, uv3, 0).xyz;
	c.xy = c.xy * 2 - vec2(1);
	c.xy *= 0.3;

	vec3 n = normalize(a + b + c).xzy;

	if(local_space)
		return n;

	// Back into world space
	n = basis * n;

	// Restore the sign
	if(geo_normal.x < 0) n.x = -n.x;
	if(geo_normal.y < 0) n.y = -n.y;
	if(geo_normal.z < 0) n.z = -n.z;

	return n;
}

/*
An animated, wet-looking surface for a blood droplet, built out of the same
water normal map get_water_normal() uses.

It is NOT get_water_normal() with a different scale, and the reason matters.
That function projects the WORLD POSITION into a basis built from the surface
normal, at 0.006 units per texel - wave features about 150 units across. A
droplet is one or two units wide, so it samples what is effectively a single
texel: a constant tilt, not a rippling surface. Worse, a droplet that MOVES
slides through a field that is fixed in the world, so the pattern crawls across
it rather than travelling with it.

A sphere has a property that removes both problems: the geometric normal IS the
direction from the droplet's centre to the surface point. Sampling off that
direction gives a pattern fixed to the droplet's own surface, needs no UVs and
no tangents, and stays centred for free as the droplet flies. Three projections
summed - the standard triplanar trick - avoid the seam a single one would leave.

`seed` is a per-droplet phase, so that a burst of sixty does not read as sixty
copies of one object.

`lod` is the mip to fetch the ripple at (pt_blood_spec_aa; 0 = full detail), and
`lost_slope_var` returns the slope variance that filtering hid, for the caller to
put back as roughness. The map's tangent slopes, measured offline on
textures/water_n.tga (2026-11-06), have E|xy|^2 = 0.0036 at mip 0, and the share
of it a 2^L box filter removes follows 1 - exp(-(L/4)^2) to within ~0.01 over
levels 1-8. The map stores z raw (~1), so its mip LENGTHS stay at 1.000 and
Toksvig-from-length (AdjustRoughnessToksvig) cannot see this loss at all.
*/
#define BLOOD_RIPPLE_SLOPE_VAR 0.0036

/*
The ripple on SURFACE BLOOD (cl_blood_film). A film tile is flat and its normals
carry how DEEP the blood is, not its shape (film_mesh, src/client/blood_film.c),
so get_blood_normal's trick - index by the geometric normal - would read one
texel over a whole pool. Index by the position in the surface plane instead, as
get_water_normal does, at pt_blood_normal_scale * BLOOD_FILM_RIPPLE_SCALE tiles
per world unit: features a few units across, a little uneven, never animated.
Two fetches, the second rotated and BLOOD_FILM_RIPPLE_SPREAD finer, so the
tiling does not show across a large pool. `lod` from the caller's footprint.
*/
#define BLOOD_FILM_RIPPLE_SCALE  0.12
#define BLOOD_FILM_RIPPLE_SPREAD 1.37

vec3 get_blood_film_normal(vec3 plane_n, vec3 position, float seed, float scale, float lod,
                           out float lost_slope_var)
{
	// Column 1 is the normal (see get_blood_normal); 0 and 2 span the plane.
	mat3 basis = construct_ONB_frisvad(plane_n);
	vec2 p = vec2(dot(position, basis[0]), dot(position, basis[2]));

	vec2 uv1 = p * scale + vec2(seed);
	vec2 uv2 = mat2(0.8, 0.6, -0.6, 0.8) * p * (scale * BLOOD_FILM_RIPPLE_SPREAD) + vec2(seed * 1.7);

	vec2 a = global_textureLod(global_ubo.water_normal_texture, uv1, lod).xy * 2 - vec2(1);
	vec2 b = global_textureLod(global_ubo.water_normal_texture, uv2, lod).xy * 2 - vec2(1);

	float strength = global_ubo.pt_blood_normal_strength * 0.7;
	vec2 perturb = (a + b) * strength;

	float lost_share = (lod > 0.0) ? 1.0 - exp(-(lod * 0.25) * (lod * 0.25)) : 0.0;
	lost_slope_var = 2.0 * strength * strength * BLOOD_RIPPLE_SLOPE_VAR * lost_share;

	vec3 n = normalize(vec3(perturb.x, 1.0, perturb.y));
	return normalize(basis * n);
}

vec3 get_blood_normal(vec3 geo_normal, float seed, bool frozen, float lod, out float lost_slope_var)
{
	// A droplet in flight has a live surface - it is a falling bead of liquid,
	// and the moving ripple is what sells that. A LANDED one does not: blood on
	// the floor has stopped moving, and animating its surface is both wrong to
	// look at and expensive in a way that does not show up as shader time. A
	// surface whose normals change every frame invalidates the denoiser's and
	// DLSS's history for every pixel covering it, so a floorful of animated
	// splats is a floorful of permanently unconverged specular. Freezing it
	// costs nothing and lets those pixels settle.
	//
	// `seed` still varies per droplet, so frozen splats do not all share one
	// pattern - they simply each keep their own.
	float t = frozen ? 0.0 : global_ubo.time * global_ubo.pt_blood_normal_speed;
	float s = global_ubo.pt_blood_normal_scale;

	vec2 uv1 = geo_normal.xy * s + vec2(seed)       + t * vec2( 0.05,  0.07);
	vec2 uv2 = geo_normal.yz * s + vec2(seed * 1.7) + t * vec2(-0.06,  0.04);
	vec2 uv3 = geo_normal.zx * s + vec2(seed * 2.3) + t * vec2( 0.03, -0.05);

	vec2 a = global_textureLod(global_ubo.water_normal_texture, uv1, lod).xy * 2 - vec2(1);
	vec2 b = global_textureLod(global_ubo.water_normal_texture, uv2, lod).xy * 2 - vec2(1);
	vec2 c = global_textureLod(global_ubo.water_normal_texture, uv3, lod).xy * 2 - vec2(1);

	float strength = global_ubo.pt_blood_normal_strength;
	vec2 perturb = (a + b + c) * strength;

	// The tangent-space normal below is (perturb.x, 1, perturb.y), so perturb IS its
	// slope: three independent fetches, each scaled by strength.
	float lost_share = (lod > 0.0) ? 1.0 - exp(-(lod * 0.25) * (lod * 0.25)) : 0.0;
	lost_slope_var = 3.0 * strength * strength * BLOOD_RIPPLE_SLOPE_VAR * lost_share;

	// Tangent space in the layout construct_ONB_frisvad returns: column 1 is the
	// normal, so the sampled normal's "up" component belongs in .y.
	vec3 n = normalize(vec3(perturb.x, 1.0, perturb.y));

	mat3 basis = construct_ONB_frisvad(geo_normal);
	return normalize(basis * n);
}

vec3 get_extinction_factors(int medium)
{
	vec3 factors = vec3(0);
	if(medium == MEDIUM_WATER)
		factors = vec3(0.035, 0.013, 0.012);
	else if(medium == MEDIUM_SLIME)
		factors = vec3(0.200, 0.010, 0.050);
	else if(medium == MEDIUM_LAVA)
		factors = vec3(0.001, 0.100, 0.300);

	return factors * global_ubo.pt_water_density;
}

vec3 extinction(int medium, float distance)
{
	vec3 factors = get_extinction_factors(medium);

	return exp(-factors * distance);
}
