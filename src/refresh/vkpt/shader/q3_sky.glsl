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

// Needs constants.h, global_ubo.h and global_textures.h first.

#ifndef Q3_SKY_GLSL_
#define Q3_SKY_GLSL_

#include "q3_blend.glsl"

/*
QUAKE III SKY CLOUDS (q3stages.c vkpt_q3_sky_upload).

A Quake III sky is a far box with cloud layers over it: scrolling images that
Quake III wrapped round a dome "cloud height" units above the eye. The far box
is the cube map (TEX_ENVMAP); the layers are drawn here, per ray, so they move -
the CPU turns the time into each layer's tcMod transform and colour every frame.

The dome is R_InitSkyTexCoords': a sphere of radius 4096 whose top is the cloud
height above the eye. Where the ray meets it, the angles of that point from the
sphere's centre are the layer's s and t, before its tcMods.

The layers blend as their blendFuncs say, in the cube map's own space: skybox
images are uploaded as plain UNORM and read as they are (the sky has always
been lit by its gamma-encoded values), and the cloud images are loaded the same
way, so the blend is the framebuffer blend Quake III did.

A QUAKE 1 SKY (q3stages.c vkpt_q1_sky_set) comes the same way, as two layers -
the back one solid, the front one see-through where its colour was 0 - with a
negative cloud height to say so. Its s and t are GLQuake's (gl_warp.c
EmitSkyPolys): the direction with its height tripled, scaled to 6*63 units,
over 128 - a sphere squashed flat, so the clouds stretch out to the horizon.

A Quake 1 sky's COLOURS are not used as they are, though. Its textures are far
darker than Quake II's or Quake III's (sky1's average is about 30/255), and
taken as linear light their darkest browns and olives - (7,7,0), (15,11,7),
next to black in Quake - come up nearly as bright as the clouds: the maroon
sky turned pink-white with olive blotches, and sky4's blue turned grey. So the
two layers are blended as GLQuake's framebuffer blended them, in their
gamma-encoded values, and the result is made linear (the sRGB curve) and
multiplied by the cloud height's size: the bake (main.c q1_sky_faces) picks
that so the sky's average brightness, and so the light it casts, is what it
was before.
*/

vec3 q1_sky_srgb_to_linear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

vec3 q3_sky_clouds(vec3 direction, vec3 farbox)
{
	if (!Q3_SHADERS)    // spec_q3 0 (global_ubo.h)
		return farbox;
	int num_layers = min(global_ubo.q3_sky_num_layers, Q3_SKY_MAX_LAYERS);
	if (num_layers <= 0)
		return farbox;

	const float R = 4096.0;
	float h = global_ubo.q3_sky_cloud_height;
	vec3 d = normalize(direction);
	vec2 st;
	if (h < 0.0)
	{
		st = d.xy * (6.0 * 63.0 / 128.0) / length(vec3(d.xy, 3.0 * d.z));
	}
	else
	{
		float p = -d.z * R + sqrt(d.z * d.z * R * R + 2.0 * R * h + h * h);
		vec3 v = normalize(d * p + vec3(0, 0, R));
		st = vec2(acos(clamp(v.x, -1.0, 1.0)), acos(clamp(v.y, -1.0, 1.0)));
	}

	vec4 dst = vec4(farbox, 1.0);
	for (int l = 0; l < num_layers; l++)
	{
		vec4 m = global_ubo.q3_sky_layers[l * Q3_SKY_LAYER_VEC4S + 0];
		vec4 k = global_ubo.q3_sky_layers[l * Q3_SKY_LAYER_VEC4S + 1];
		vec4 colour = global_ubo.q3_sky_layers[l * Q3_SKY_LAYER_VEC4S + 2];
		uint image = floatBitsToUint(k.z);
		uint blend = floatBitsToUint(k.w);

		vec2 uv = vec2(m.x * st.x + m.y * st.y + k.x,
		               m.z * st.x + m.w * st.y + k.y);
		vec4 src = global_textureLod(image, uv, 0.0) * colour;

		vec4 sf = q3_blend_factor(blend & 0xfu, src, dst);
		vec4 df = q3_blend_factor((blend >> 4) & 0xfu, src, dst);
		dst = clamp(src * sf + dst * df, vec4(0), vec4(1));
	}
	if (h < 0.0)    // Quake 1: its blend made linear, at the bake's scale
		return q1_sky_srgb_to_linear(dst.rgb) * -h;
	return dst.rgb;
}

/*
QUAKE III CLOUD SHADOWS (pt_q3_cloud_shadow; q3stages.c q3_cloud_shadow_upload).

The sky's clouds laid flat over the world, so the map's own sun (q3map_sun)
comes through them: where the clouds between a point and the sun are thick,
less light. Matt, 2026-11-26: "when the red clouds pass over the sun we can
attenuate the light based on the thickness of the red clouds", for everything
outside, and "it should be the clouds from the skybox when they cover the sun".

A point looks up along the sun to the clouds. Flat, their height only moves
the whole pattern, so the CPU folds it into one scale: p.x turns world units
into the dome's s and t - near its top, 1 / (R + cloud height) per sky unit,
as q3_sky_clouds' acos does, times the sky units per world unit that
pt_q3_cloud_shadow_height picks. There ALL the sky's layers are read, with
this frame's tcMods, and blended as q3_sky_clouds blends them - the clouds
the sky shows, moving as they do - at a coarser mip (k.y,
pt_q3_cloud_shadow_soft) for soft edges.

How thick: Matt, "The blue is the sky. The rest is clouds and the orange
parts are the thickest parts". So the blend's warmth, (r + g) / 2 - b, in the
gamma-encoded values Quake III blended (an image read decoded is encoded
again, Q3_SKY_LAYER_SRGB), from its 10th percentile (p.y, none) to its 95th
(p.z, the thickest) - q3stages.c sky_cloud_warmth. A first try took one
layer's brightness: terrain1's blue is its second layer, and only the red
one's brightest specks counted - small shadows that went from nothing to
pitch black, and got smaller rather than lighter with less strength.

THE SKY'S LIGHT TOO. Taking only the sun could not be seen: Matt, on the first
run, "I set the strength to 1,000 and could not see it" - on Team Arena's
terrain most of the light outside is the sky's (q3map_surfacelight, its sky
light polygons), and the sun is the smaller part (pt_q3_sun_scale). The
terrain's own fake multiplied its clouds over all the light on it, and Matt
wanted it "at least as strong as it was with the fake effect", on "all
geometry in the sun". So the sky's light that reaches a point - its sky light
polygons (sample_lights, process_selected_light_restir) and a bounce ray that
finds the sky (indirect_lighting.rgen) - goes through the same clouds, looked
up the same way, along the sun: one pattern, moving with the sun's shadows.

The thickest cloud takes k.x of the sun and p.w of the sky's light
(pt_q3_cloud_shadow_strength, times pt_q3_cloud_shadow_sky for the sky), never
more than 0.9: "we need visible dimming without pitch black shadows". p.x is 0
when they are off.
*/

vec3 q3_cloud_srgb_encode(vec3 c)
{
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

// how thick the clouds are over this point, along the sun: 0 open sky, 1 the
// thickest; -1 when there are none (pt_q3_cloud_shadow 0)
float q3_cloud_thickness(vec3 position)
{
	if (!Q3_SHADERS)
		return -1.0;
	vec4 p = global_ubo.q3_cloud_shadow_p;
	if (p.x == 0.0)
		return -1.0;
	int num_layers = min(global_ubo.q3_sky_num_layers, Q3_SKY_MAX_LAYERS);
	float lod = global_ubo.q3_cloud_shadow_k.y;

	vec3 to_sun = global_ubo.sun_direction;
	vec2 at = position.xy - to_sun.xy * (position.z / max(to_sun.z, 0.05));
	vec2 st = vec2(0.5 * M_PI) - at * p.x;

	vec4 dst = vec4(0, 0, 0, 1);
	for (int l = 0; l < num_layers; l++)
	{
		vec4 m = global_ubo.q3_sky_layers[l * Q3_SKY_LAYER_VEC4S + 0];
		vec4 k = global_ubo.q3_sky_layers[l * Q3_SKY_LAYER_VEC4S + 1];
		vec4 colour = global_ubo.q3_sky_layers[l * Q3_SKY_LAYER_VEC4S + 2];
		uint blend = floatBitsToUint(k.w);

		vec2 uv = vec2(m.x * st.x + m.y * st.y + k.x,
		               m.z * st.x + m.w * st.y + k.y);
		vec4 tex = global_textureLod(floatBitsToUint(k.z), uv, lod);
		if ((blend & Q3_SKY_LAYER_SRGB) != 0u)
			tex.rgb = q3_cloud_srgb_encode(max(tex.rgb, vec3(0)));
		vec4 src = tex * colour;

		vec4 sf = q3_blend_factor(blend & 0xfu, src, dst);
		vec4 df = q3_blend_factor((blend >> 4) & 0xfu, src, dst);
		dst = clamp(src * sf + dst * df, vec4(0), vec4(1));
	}

	float warmth = 0.5 * (dst.r + dst.g) - dst.b;
	return clamp((warmth - p.y) / max(p.z - p.y, 1e-4), 0.0, 1.0);
}

// what the clouds leave of the sun's light here
float q3_cloud_shadow(vec3 position)
{
	float thickness = q3_cloud_thickness(position);
	return thickness <= 0.0 ? 1.0 : max(1.0 - global_ubo.q3_cloud_shadow_k.x * thickness, 0.0);
}

// and of the sky's (pt_q3_cloud_shadow_sky)
float q3_cloud_sky_shadow(vec3 position)
{
	float thickness = q3_cloud_thickness(position);
	return thickness <= 0.0 ? 1.0 : max(1.0 - global_ubo.q3_cloud_shadow_p.w * thickness, 0.0);
}

#endif // Q3_SKY_GLSL_
