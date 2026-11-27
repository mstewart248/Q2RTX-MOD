/*
Copyright (C) 2026 Matt Stewart

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

// Needs constants.h and global_ubo.h first. Shared by fog_medium.glsl and
// god_rays_filter.comp, which does not include the whole medium.

#ifndef FOG_Q3_GLSL_
#define FOG_Q3_GLSL_

/*
QUAKE III FOG BRUSHES.

fog.c uploads a Quake III map's fog brushes into global_ubo.fog_volumes[] with
is_active 2, after the "fog" command's own boxes (is_active 1, which belong to
the path tracer's ray tint and not to this medium). Each is a box of the map's
fogparms colour whose density.w is a REAL extinction per unit - 3 / the
distance Quake III draws it opaque at.

They join the medium twice over: as density (q3_fog_density, scaled by
pt_fog_q3_density into the medium's own units so the lights and the sky
scatter in it like any other fog) and as true extinction (q3_fog_sigma, which
froxel_scatter.comp and god_rays.comp add to the transmittance, so a fog brush
hides what is behind it the way the Quake III one does).

The faces are softened over Q3_FOG_EDGE units so a froxel straddling one does
not flicker in and out as its sample point is jittered.

THE TOP FADES IN (pt_fog_q3_fade, fog.c puts it in density.x): the density is 0
at the brush's top face and grows linearly to full that many units below it.
Matt, 2026-11-17, q3dm4's pit: "fall all the way off at the top of the brush"
and full "around the player height at the bottom of the stairs". That brush is
128 units tall with its floor at its bottom, so the default 72 puts full fog at
56 units above the floor, a player's height. Measured from the TOP, not from
the floor, because other maps' brushes reach far below their floors (q3dm11's
by 370 units) and a floor-anchored ramp would thin their whole fog. 0 = the old
hard top.
*/
#define Q3_FOG_EDGE 8.0

/* NO FOG BRUSH IN THE WATER (Matt, 2026-11-27: "I just don't want q3 fog brushes
   rendered in water"). A leg of the path that runs through water, slime or lava -
   the eye's column while the eye is under the surface, the view down through a
   liquid's surface, a bounce under it - sees no fog brush at all: q3_fog_at reads
   0 and q3_fog_transmittance 1 while the caller has this set. A leg in the air
   sees every brush as before, the part of one that hangs above a liquid included
   ("The fog brushes should render if they are not in water"). god_rays.comp sets
   it for its march from the leg's medium (pass 0's is the eye's, primary_rays
   stamps it), god_rays_filter around the eye's column (global_ubo.medium) and
   bounce 0's leg, the froxel passes for the whole grid (the eye's column),
   reflect_refract.rgen tests its own leg's medium. A brush that IS a liquid, or
   sits wholly under one, is left out at load (bsp_q3.c, map_q3_fog_in_liquid).
   Nothing else about the water changes. */
bool q3_fog_leg_in_liquid = false;

bool q3_fog_medium_is_liquid(int medium)
{
	return medium == MEDIUM_WATER || medium == MEDIUM_SLIME || medium == MEDIUM_LAVA;
}

// fraction of a fog brush's density at depth u below its top, for a fade of depth f
float q3_fog_fade(float u, float f)
{
	return (f > 0.0) ? clamp(u / f, 0.0, 1.0) : 1.0;
}

// the integral of q3_fog_fade from the top down to depth u >= 0
float q3_fog_fade_integral(float u, float f)
{
	if (f <= 0.0)
		return u;
	return (u < f) ? u * u / (2.0 * f) : u - 0.5 * f;
}

// a fog brush's colour at the luminance of white: its hue, the brightness taken out
vec3 q3_fog_tint(vec3 color)
{
	return color / max(dot(color, vec3(0.2126, 0.7152, 0.0722)), 1e-3);
}

float q3_fog_at(vec3 p, out vec3 color)
{
	float sigma = 0.0;
	vec3  c = vec3(0.0);
	if (!Q3_SHADERS || q3_fog_leg_in_liquid)    // spec_q3 0 (global_ubo.h): no fog brushes
	{
		color = vec3(1.0);
		return 0.0;
	}
	for (int i = 0; i < MAX_FOG_VOLUMES; i++)
	{
		if (global_ubo.fog_volumes[i].is_active == 0u)
			break;
		if (global_ubo.fog_volumes[i].is_active != 2u)
			continue;
		vec3 inside = min(p - global_ubo.fog_volumes[i].mins, global_ubo.fog_volumes[i].maxs - p);
		float edge = min(min(inside.x, inside.y), inside.z);
		if (edge <= 0.0)
			continue;
		float s = global_ubo.fog_volumes[i].density.w * clamp(edge / Q3_FOG_EDGE, 0.0, 1.0)
		        * q3_fog_fade(global_ubo.fog_volumes[i].maxs.z - p.z, global_ubo.fog_volumes[i].density.x);
		sigma += s;
		c += global_ubo.fog_volumes[i].color * s;
	}
	color = (sigma > 0.0) ? c / sigma : vec3(1.0);
	return sigma;
}

// true extinction per unit at p from the map's fog brushes
float q3_fog_sigma(vec3 p)
{
	vec3 c;
	return q3_fog_at(p, c);
}

// whether any fog brush is uploaded at all - the filter's switch for applying
// transmittance to the picture when the map fog itself has no extinction
bool q3_fog_present()
{
	if (!Q3_SHADERS)
		return false;
	for (int i = 0; i < MAX_FOG_VOLUMES; i++)
	{
		uint a = global_ubo.fog_volumes[i].is_active;
		if (a == 0u)
			return false;
		if (a == 2u)
			return true;
	}
	return false;
}

/* A light's volumetric scale at p. A Quake III map light left at its class
   default arrives NEGATIVE (vertex_buffer.c resolve_volumetric_scale): it fogs
   inside the map's fog brushes, which it has to light, and nowhere else. */
float fog_light_vol_scale(float s, vec3 p)
{
	if (s >= 0.0)
		return s;
	return (q3_fog_sigma(p) > 0.0) ? -s : 0.0;
}

/* How much of what is behind the fog brushes survives them, along a straight
   ray from `origin` for `dist` units: each box's extinction times the length of
   the ray inside it, weighted by the top's fade (exactly: the fade is linear in
   depth, so its integral along the ray is a difference of q3_fog_fade_integral
   over the change in depth - the midpoint where the ray is near level, which
   the difference cannot divide by). Exact for the boxes (their soft Q3_FOG_EDGE faces aside),
   and used for the picture instead of the froxel grid's transmittance because
   that also carries the map fog's token extinction, which is too much to dim
   the whole scene by - see god_rays_filter.comp. `color` is the boxes' fogparms
   colour, weighted by how much of the ray's fog each one is. */
float q3_fog_transmittance(vec3 origin, vec3 dir, float dist, out vec3 color)
{
	float optical = 0.0;
	vec3 c = vec3(0.0);
	if (!Q3_SHADERS || q3_fog_leg_in_liquid)
	{
		color = vec3(1.0);
		return 1.0;
	}
	vec3 inv_dir = vec3(1.0) / dir;
	for (int i = 0; i < MAX_FOG_VOLUMES; i++)
	{
		if (global_ubo.fog_volumes[i].is_active == 0u)
			break;
		if (global_ubo.fog_volumes[i].is_active != 2u)
			continue;
		vec3 t1 = (global_ubo.fog_volumes[i].mins - origin) * inv_dir;
		vec3 t2 = (global_ubo.fog_volumes[i].maxs - origin) * inv_dir;
		vec3 tmin = min(t1, t2), tmax = max(t1, t2);
		float t_in  = max(max(max(tmin.x, tmin.y), tmin.z), 0.0);
		float t_out = min(min(min(tmax.x, tmax.y), tmax.z), dist);
		if (t_out > t_in)
		{
			float f   = global_ubo.fog_volumes[i].density.x;
			float top = global_ubo.fog_volumes[i].maxs.z;
			float u0  = max(top - (origin.z + dir.z * t_in), 0.0);
			float u1  = max(top - (origin.z + dir.z * t_out), 0.0);
			float len = (abs(u1 - u0) < 0.5)
				? (t_out - t_in) * q3_fog_fade(0.5 * (u0 + u1), f)
				: (q3_fog_fade_integral(u1, f) - q3_fog_fade_integral(u0, f)) / (u1 - u0) * (t_out - t_in);
			float o = global_ubo.fog_volumes[i].density.w * len;
			optical += o;
			c += global_ubo.fog_volumes[i].color * o;
		}
	}
	color = (optical > 0.0) ? c / optical : vec3(1.0);
	return exp(-optical);
}

#endif // FOG_Q3_GLSL_
