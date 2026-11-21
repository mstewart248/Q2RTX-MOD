/*
Copyright (C) 2026

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

STAINED GLASS (Matt, 2026-11-21: Quake 1's window02_1 in start.bsp)

"kind STAINEDGLASS" (material.c) = REGULAR + stained_glass 1 + is_light 1. The
texture is a pane of thick coloured glass with a made-up light behind it - a
lamp, or the sun - and nothing behind the wall is ever traced: the surface
stays opaque. Quake 1's windows are a texture on a solid wall.

WHERE THE LIGHT IS. One texture tile is one window (every window in id1's maps
is drawn as exactly one tile, or part of one). The light sits behind the tile:
stained_light_height up it (1 = level with the top edge, 0 the bottom - "top"
being the top of the picture, v = 0), in the middle across, and
stained_light_distance tile heights back from the glass. stained_light_sun 1
makes it a sun instead: parallel light along the line from that spot to the
middle of the tile. stained_light_size is its radius as seen from the middle of
the tile, in degrees - the softness of the window's image on the floor. (The
sun looks small, so its image is sharp; a lamp of the same size closer in
looks bigger and casts a softer one.)

HOW BRIGHT. emissive_factor x the surface's emissive scale (default_radiance,
or the face's light value) sets the light's IRRADIANCE on the glass at the
middle of the tile: pi x that, so that fully spread glass (stained_scatter 1)
glows as bright as an ordinary emissive texture of the same emissive_factor.
The light's own radiance follows from its size (stained_light_radiance). A
lamp lights the glass less towards the far edges (1/d^2 and the slant); a sun
lights it evenly.

WHAT THE GLASS SHOWS (stained_glass_radiance, from get_material). The base
texture is the glass's TRANSMISSION, T, per colour; the lead is where T is
dark. The light through the glass is split by stained_scatter, s:
  * s of it is spread by the glass (antique glass): half glows evenly from
    every piece, half in a bloom round the light's way through, whose
    half-width is stained_spread. Looking along the light, the glass is lit up
    from inside round where the light is.
  * 1 - s goes straight through: the light itself is seen through the glass,
    a spot where the eye, the glass and the light line up.
Both are bent by the glass's normal map (the bumps refract the light's way
through), coloured by T, deeper at a slant (the path through the glass is
longer, T^(1/cos)), and lose the Fresnel reflection at the two faces.

WHAT LIGHTS THE ROOM.
  * The straight share, 1 - s: a WINDOW LIGHT (DYNLIGHT_WINDOW, one per window:
    the faces on one tile of one plane, built in bsp_mesh.c; world faces only,
    a door's or a train's get the fill alone). A point in the room is lit by the light
    through the one spot on the glass on the line to the light, in that spot's
    colour - so the floor gets an image of the window, sharp for a small light.
    sample_window_light() picks a point on the light, finds where the line to
    it crosses the glass, and the shadow ray stops just short of the glass.
  * The spread share, s: an ordinary emissive light poly over the faces, in
    the texture's average colour (soft fill).

==============================================================================
*/

#ifndef _STAINED_GLASS_H_
#define _STAINED_GLASS_H_

/* COMPILED OUT on a map without it: behind spec_q3 (global_ubo.h), which
   vertex_buffer.c turns on for any stained glass material, as it does for the
   other glass features. The light sampling and the look spilled the ray
   shaders' registers (indirect lighting 96 -> 160 B local memory). */
#ifdef Q3_SHADERS
#define STAINED_GLASS_CODE Q3_SHADERS
#else
#define STAINED_GLASS_CODE true
#endif

#define STAINED_ETA          1.5
// of the light the glass spreads, the share in the bloom round the light's
// way through; the rest glows evenly
#define STAINED_BLOOM_SHARE  0.5
// the light seen straight through the glass is drawn at least this big (sin of
// its radius, ~1.5 degrees) at the same total, so a small sun is not one
// unresolvable pixel
#define STAINED_MIN_SPOT     0.026

// The light behind one tile of stained glass.
struct StainedLight
{
	vec3 centre;      // the middle of the tile, on the glass
	vec3 lamp;        // the lamp's centre (where the sun is seen from, for a sun)
	vec3 to_light;    // from the middle of the tile towards the light, unit
	vec3 n_room;      // the glass's normal, on the room's side
	float dist_c;     // the middle of the tile to the lamp
	float sin_size;   // sin of the light's radius seen from the middle of the tile
	float radius;     // the lamp's radius
	float cos_c;      // the light's slant onto the glass at the middle of the tile
	float radiance;   // the light's radiance, for an irradiance of 1 on the glass at the middle
	bool sun;
};

/* The light behind the tile whose top-left corner (u, v = 0, 0) is `corner`,
   whose u and v run along e_u and e_v (world units per tile), on the glass
   facing n_room. */
StainedLight
stained_light(vec3 corner, vec3 e_u, vec3 e_v, vec3 n_room, MaterialInfo m)
{
	StainedLight s;
	float tile_h = max(length(e_v), 1e-3);
	s.n_room = n_room;
	s.centre = corner + 0.5 * e_u + 0.5 * e_v;
	// v runs down the picture: height 1 is the tile's top edge (v 0), 0 its bottom (v 1)
	vec3 spot = corner + 0.5 * e_u + (1.0 - m.stained_height) * e_v;
	s.lamp = spot - n_room * (max(m.stained_distance, 0.01) * tile_h);
	vec3 d = s.lamp - s.centre;
	s.dist_c = max(length(d), 1e-3);
	s.to_light = d / s.dist_c;
	s.sun = m.stained_sun;
	s.sin_size = sin(clamp(m.stained_size, 1e-3, 1.0));
	s.radius = s.dist_c * s.sin_size;
	s.cos_c = max(dot(s.to_light, -n_room), 0.05);
	// irradiance on the glass at the middle = radiance x pi sin^2 x the slant
	s.radiance = 1.0 / (M_PI * s.sin_size * s.sin_size * s.cos_c);
	return s;
}

/* The light's irradiance on the glass at p - 1 at the middle of the tile - and
   the way it travels there (from the light, through the glass, unit). */
float
stained_irradiance(StainedLight s, vec3 p, out vec3 travel)
{
	if (s.sun)
	{
		travel = -s.to_light;
		return 1.0;
	}
	vec3 d = s.lamp - p;
	float dist2 = max(dot(d, d), 1e-4);
	vec3 l = d * inversesqrt(dist2);
	travel = -l;
	float cos_p = max(dot(l, -s.n_room), 0.0);
	return (s.dist_c * s.dist_c / dist2) * (cos_p / s.cos_c);
}

// The share of the light that gets through one face of the glass at cosine c
// (Schlick, F0 0.04)
float
stained_face_transmit(float c)
{
	float x = 1.0 - clamp(c, 0.0, 1.0);
	float x2 = x * x;
	return 1.0 - (0.04 + 0.96 * x2 * x2 * x);
}

// The glass's colour T for light crossing it at cosine c to the normal: the
// path inside is 1 / cos(refracted angle) the thickness, so a slant deepens it
vec3
stained_thick_transmit(vec3 T, float c)
{
	float sin2_t = (1.0 - c * c) * (1.0 / (STAINED_ETA * STAINED_ETA));
	float cos_t = sqrt(max(1.0 - sin2_t, 0.3));
	return pow(clamp(T, vec3(0), vec3(1)), vec3(1.0 / cos_t));
}

/* What the glass shows at p towards w_o (unit, from the glass to the eye), for
   an irradiance of 1 at the middle of the tile. T is the base texture there
   (the glass's colour), n_shade the normal map's normal. */
vec3
stained_glass_radiance(StainedLight s, MaterialInfo m, vec3 p, vec3 n_shade, vec3 w_o, vec3 T)
{
	vec3 travel;
	float E = stained_irradiance(s, p, travel);
	float cos_o = dot(w_o, s.n_room);
	if (E <= 0.0 || cos_o <= 0.0)
		return vec3(0);

	// the bumps of the glass bend the light on its way out: a face tilted by
	// (n_shade - n_room) turns it (eta - 1) times that away from the tilt
	vec3 f = normalize(travel - (STAINED_ETA - 1.0) * (n_shade - s.n_room));
	float c = dot(w_o, f);

	vec3 Tv = stained_thick_transmit(T, cos_o)
		* (stained_face_transmit(cos_o) * stained_face_transmit(dot(travel, s.n_room)));

	// the spread share: half an even glow (1/pi), half a bloom about f whose
	// half-width is stained_spread, normalised to the same total
	float kappa = 0.6931472 / max(1.0 - cos(m.stained_spread), 1e-4);
	float norm = kappa * kappa / (2.0 * (kappa - 1.0 + exp(-kappa)));
	float bloom = norm * exp(kappa * (min(c, 1.0) - 1.0));
	float lobe = ((1.0 - STAINED_BLOOM_SHARE) + STAINED_BLOOM_SHARE * bloom) * (1.0 / M_PI);
	vec3 L = Tv * (E * m.stained_scatter * lobe);

	// the straight share: the light itself, seen through the glass
	float clear = 1.0 - m.stained_scatter;
	if (clear > 0.0)
	{
		float sin_a = s.sun ? s.sin_size : min(s.radius * inversesqrt(max(dot(s.lamp - p, s.lamp - p), 1e-4)), 1.0);
		float sin_vis = max(sin_a, STAINED_MIN_SPOT);
		float one_minus_cos = 1.0 - sqrt(max(1.0 - sin_vis * sin_vis, 0.0));
		float spot = smoothstep(1.0 - 1.5 * one_minus_cos, 1.0 - 0.5 * one_minus_cos, c);
		L += Tv * (s.radiance * clear * spot * (sin_a * sin_a) / (sin_vis * sin_vis));
	}

	return L;
}

/* A tile's corner and u/v edges in the world, from a triangle on it: its
   corners, their texture coordinates, and a point p on it whose texture
   coordinate is uv. False for a triangle with no texture mapping. */
bool
stained_tile_frame(mat3 positions, mat3x2 tex_coords, vec3 p, vec2 uv, out vec3 corner, out vec3 e_u, out vec3 e_v)
{
	vec3 dp1 = positions[1] - positions[0];
	vec3 dp2 = positions[2] - positions[0];
	vec2 duv1 = tex_coords[1] - tex_coords[0];
	vec2 duv2 = tex_coords[2] - tex_coords[0];
	float det = duv1.x * duv2.y - duv2.x * duv1.y;
	corner = p;
	e_u = vec3(0);
	e_v = vec3(0);
	if (abs(det) < 1e-12)
		return false;
	float inv = 1.0 / det;
	e_u = (dp1 * duv2.y - dp2 * duv1.y) * inv;
	e_v = (dp2 * duv1.x - dp1 * duv2.x) * inv;
	vec2 f = uv - floor(uv);
	corner = p - f.x * e_u - f.y * e_v;
	return true;
}

/* A WINDOW LIGHT (DYNLIGHT_WINDOW): the straight-through share of the light
   behind one window - the faces of one tile of stained glass on one plane -
   as a light the room is lit by. Its record (bsp_mesh.c, vertex_buffer.c
   copy_light):
     positions[0], [1]   S, T, in the glass's plane: a point x on the glass is
                         at u = dot(x, S) + window.x, v = dot(x, T) + window.y
                         in the tile (0..1 = the tile; the texture repeats)
     positions[2].xy     the glass's extent in the tile, u0..u1 and v0..v1, each
                         pair packed 12 + 12 bits over -1..2 (a window can be a
                         part of its tile, or run a little over its edge); the
                         light is let through there only, not through the wall
                         round it
     window.z            the glass's plane: dot(x, n) = window.z, n facing the room
     spot_emission_profile  material index x 2, + 1 if n = -(S x T)
     color               the irradiance on the glass at the middle of the tile
                         (pi x emissive_factor x its scale x light_factor) */
struct WindowLight
{
	vec3 S;
	vec3 T;
	vec3 n;
	vec3 window;      // u offset, v offset, the glass's plane distance
	vec4 rect;        // u0, v0, u1, v1
	uint material;
};

float
window_rect_unpack(uint q)
{
	return float(q) * (3.0 / 4095.0) - 1.0;
}

WindowLight
get_window_light(LightPolygon light)
{
	WindowLight w;
	w.S = light.positions[0];
	w.T = light.positions[1];
	uint pm = uint(light.spot_emission_profile + 0.5);
	w.material = pm >> 1;
	vec3 n = normalize(cross(w.S, w.T));
	w.n = (pm & 1u) != 0u ? -n : n;
	w.window = light.window;
	uint ru = uint(light.positions[2].x + 0.5);
	uint rv = uint(light.positions[2].y + 0.5);
	w.rect = vec4(window_rect_unpack(ru & 0xfffu), window_rect_unpack(rv & 0xfffu),
	              window_rect_unpack(ru >> 12), window_rect_unpack(rv >> 12));
	return w;
}

// the light behind the window, from its record
StainedLight
window_stained_light(WindowLight w, MaterialInfo m)
{
	// S and T lie in the glass's plane (bsp_mesh.c puts them there), so the
	// tile's edges are their duals in it: e_u . S = 1, e_u . T = 0, e_u . n = 0
	vec3 c = cross(w.S, w.T);
	float inv_det = 1.0 / dot(c, w.n);
	vec3 e_u = cross(w.T, w.n) * inv_det;
	vec3 e_v = cross(w.n, w.S) * inv_det;
	vec3 corner = w.n * w.window.z - e_u * w.window.x - e_v * w.window.y;
	return stained_light(corner, e_u, e_v, w.n, m);
}

/* How much of the window light p gets, unshadowed and untextured - the mass
   sample_lights() and ReSTIR's target pick it by. The light's solid angle (as
   the sphere light's), times its radiance per unit `color`, times the straight
   share; 0 if the line to the light's centre misses the tile (with the light's
   own size as a margin). */
float
window_light_mass_volume(LightPolygon light, vec3 p, out vec3 dir)
{
	dir = vec3(0, 0, 1);
	if (!STAINED_GLASS_CODE)
		return 0.0;

	WindowLight w = get_window_light(light);
	MaterialInfo m = get_material_info(w.material);
	StainedLight s = window_stained_light(w, m);

	dir = s.to_light;
	float h = dot(p, w.n) - w.window.z;
	if (h <= 0.01)
		return 0.0;

	float solid_angle;
	float margin;
	if (s.sun)
	{
		solid_angle = 2.0 * M_PI * (1.0 - sqrt(max(1.0 - s.sin_size * s.sin_size, 0.0)));
		margin = h * s.sin_size;
	}
	else
	{
		vec3 d = s.lamp - p;
		float dist = length(d);
		dir = d / dist;
		float sin_a = min(s.radius / dist, 1.0);
		solid_angle = 2.0 * M_PI * (1.0 - sqrt(max(1.0 - sin_a * sin_a, 0.0)));
		margin = s.radius;
	}

	float denom = dot(dir, w.n);
	if (denom >= -1e-4)
		return 0.0;
	vec3 g = p + dir * (-h / denom);
	vec2 uv = vec2(dot(g, w.S) + w.window.x, dot(g, w.T) + w.window.y);
	vec2 m_uv = margin * vec2(length(w.S), length(w.T)) / max(-denom, 0.1);
	if (any(lessThan(uv, w.rect.xy - m_uv)) || any(greaterThan(uv, w.rect.zw + m_uv)))
		return 0.0;

	return solid_angle * s.radiance * (1.0 - m.stained_scatter);
}

// the same at a surface with normal n, which must face the light
float
window_light_mass(LightPolygon light, vec3 p, vec3 n)
{
	vec3 dir;
	float m = window_light_mass_volume(light, p, dir);
	return dot(dir, n) > 0.0 ? m : 0.0;
}

/* One sample of the window light at p. Returns the point the shadow ray goes
   to: where the line to the sampled point on the light crosses the glass,
   pulled half a unit back towards p, so the glass does not shadow its own
   light. `tint` is what comes through there per unit `color` - the glass's
   colour at that spot, its slant and Fresnel losses, the light's radiance and
   the straight share - already divided by pdfw; pdfw 0 = nothing (behind the
   glass, or through the wall beside the tile). */
vec3
sample_window_light(vec3 p, LightPolygon light, vec2 rnd, out vec3 tint, out float pdfw)
{
	tint = vec3(0);
	pdfw = 0.0;
	if (!STAINED_GLASS_CODE)
		return p;

	WindowLight w = get_window_light(light);
	MaterialInfo m = get_material_info(w.material);
	StainedLight s = window_stained_light(w, m);

	float h = dot(p, w.n) - w.window.z;
	if (h <= 0.01)
		return p;

	vec3 dir;
	float blur;      // how far the light's size spreads a point of the glass, per unit from it
	if (s.sun)
	{
		mat3 onb = construct_ONB_frisvad(s.to_light);
		float tan_size = s.sin_size * inversesqrt(max(1.0 - s.sin_size * s.sin_size, 1e-6));
		vec2 disk = sample_disk(rnd) * tan_size;
		dir = normalize(s.to_light + onb[0] * disk.x + onb[2] * disk.y);
		pdfw = 1.0 / (2.0 * M_PI * (1.0 - sqrt(max(1.0 - s.sin_size * s.sin_size, 0.0))));
		blur = tan_size;
	}
	else
	{
		vec3 ln;
		vec3 q = sample_projected_sphere(p, mat3(s.lamp, vec3(s.radius, 0, 0), vec3(0)), rnd, ln, pdfw);
		vec3 d = q - p;
		float dist = length(d);
		dir = d / dist;
		blur = s.radius / max(dist, 1e-3);
	}

	float denom = dot(dir, w.n);
	if (!(pdfw > 0.0) || denom >= -1e-4)
	{
		pdfw = 0.0;
		return p;
	}

	float t = -h / denom;
	vec3 g = p + dir * t;
	vec2 uv = vec2(dot(g, w.S) + w.window.x, dot(g, w.T) + w.window.y);
	if (any(lessThan(uv, w.rect.xy)) || any(greaterThan(uv, w.rect.zw)))
	{
		pdfw = 0.0;
		return p;
	}

	// The glass's colour at the crossing. The light's size blurs the window's
	// image by blur * t at p - read the texture at about that footprint, so a
	// soft image is not a noisy one.
	vec3 T = vec3(1);
	if (m.base_texture != 0)
	{
		ivec2 size = global_textureSize(m.base_texture, 0);
		float texels = blur * t * max(length(w.S) * float(size.x), length(w.T) * float(size.y));
		float lod = clamp(log2(max(texels, 1.0)) - 1.0, 0.0, 8.0);
		T = global_textureLod(m.base_texture, uv, lod).rgb;
	}
	T = clamp(T * m.base_factor, vec3(0), vec3(1));

	float cos_g = -denom;
	tint = stained_thick_transmit(T, cos_g) * (stained_face_transmit(cos_g) * stained_face_transmit(cos_g))
		* (s.radiance * (1.0 - m.stained_scatter) / pdfw);

	return g - dir * 0.5;
}

#endif // _STAINED_GLASS_H_
