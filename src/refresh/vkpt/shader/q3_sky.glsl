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

#endif // Q3_SKY_GLSL_
