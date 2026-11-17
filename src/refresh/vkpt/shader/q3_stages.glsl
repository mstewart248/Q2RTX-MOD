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

// Needs constants.h, vertex_buffer.h (light_buffer, Triangle) and
// global_textures.h first. Included by path_tracer_rgen.h.

#ifndef Q3_STAGES_GLSL_
#define Q3_STAGES_GLSL_

/*
QUAKE III SHADER STAGES (q3stages.c).

Quake III drew a surface by blending its stages into the framebuffer one after
another: the lightmap, the texture multiplied over it, a glow added on top, a
pulse scrolling along that. A path tracer needs three things instead - what the
surface REFLECTS (albedo, which the light at the hit then multiplies), what it
EMITS, and how much of what is behind it shows THROUGH.

All three fall out of running the same blend three times. Call the light at the
surface L (what $lightmap stood for) and what is behind it B. Every Quake III
blend is linear in each of them, so with

	C00 = the stages blended with L = 0, B = 0
	C10 =                         L = 1, B = 0
	C11 =                         L = 1, B = 1

the emission is C00 (what is there with no light and nothing behind), the albedo
is C10 - C00 (what one unit of light adds), and the transmission is C11 - C10
(what one unit of background adds). A stage that fails its alphaFunc is left out
of all three. The CPU has already turned the time into each stage's frame,
colour and texture-coordinate matrices; what is left here depends on the hit.
*/

// Set by get_material: how much of a Quake III program's surface covers what is
// behind it at this hit (1 - transmission), or -1 when it has no program or is
// opaque. primary_rays / reflect_refract take it as the triangle's alpha.
float q3_surface_alpha = -1.0;

/* Set by q3_program_material under pt_q3_env_shine (Q3P_PROG_SHINE, see
   constants.h): whether it is on for this surface, how much its reflection-map
   stages showed at this hit (0..1), and whether the shader is metal.
   get_material applies the material's roughness_override / metalness_factor
   with them. */
bool q3_shine_on = false;
float q3_shine = 0.0;
bool q3_shine_metal = false;

/* STAGE SETTINGS (q3stages.h, Q3P_PROG_PROPS): a stage with a surface of its
   own - the .mat's stage<N>_roughness_override, _texture_normals, ... - lays it
   over the material's where the stage shows (Matt, 2026-11-16: q3dm2's
   q1metal7_99spot floor is bricks cut out over a pool of grue, and the bricks
   took the grue's slippery metal). Each property is a sum over the stages that
   state it, weighted by how much of the surface each one is by the end, and
   that weight W; get_material ends with sum + (1 - W) x the material's own.

   How much of the surface a stage is, t, comes from its blend:
   - laid over what is there (opaque, alpha blended): 1 - its destination
     factor - its alpha, for a blend;
   - added on top: its share of the brightness, if it states anything (an
     addition stating nothing leaves the surface what it was);
   - multiplied in (a filter): all of it where it is drawn, if it states
     anything - a texture filtered over the lightmap IS the surface - and
     nothing if it does not;
   - the lightmap: the light, never a surface.
   A stage stating a property takes its t of it; one that covers the surface
   without stating it hands its t of that property back to the material. */
vec4 q3_prop_sum = vec4(0);     // roughness, metalness, specular factor, -
vec4 q3_prop_w = vec4(0);       // ... their weights, and the normal's
vec3 q3_prop_normal = vec3(0);  // tangent space, weighted

uint q3_program_offset(uint material_id)
{
	// spec_q3 0 (global_ubo.h): no programs, and all of the code below goes
	if (!Q3_SHADERS)
		return 0u;
	uint index = material_id & MATERIAL_INDEX_MASK;
	return light_buffer.material_table[index * MATERIAL_UINTS + 9] >> 16;
}

#include "q3_blend.glsl"

/* The share of the colour so far that a reflection map made, carried through a
   stage the way its blend carries the colour: `w` is the stage's own share (a
   reflection map's strength, 0 for anything else), weighted as the colour is.
   A stage that only scales what is there - a lightmap multiply - leaves it. */
float q3_shine_share(float share, float w, vec4 s, vec4 d, uint src_f, uint dst_f)
{
	if (src_f == Q3P_BF_ZERO || src_f == Q3P_BF_DST_COLOR || src_f == Q3P_BF_ONE_MINUS_DST_COLOR)
		return share;
	vec3 fs = q3_blend_factor(src_f, s, d).rgb;
	vec3 fd = q3_blend_factor(dst_f, s, d).rgb;
	return w * dot(fs, vec3(1.0 / 3.0)) + share * dot(fd, vec3(1.0 / 3.0));
}

/* One of a stage's images at its texture coordinates `st`; `lin` is the linear
   part of its tcMods, for the footprint. `mip_level` < 0: with the gradients. */
vec4 q3_stage_sample(uint image, vec2 st, mat2 lin, bool env, float mip_level, vec2 base_size,
	vec2 tex_coord_x, vec2 tex_coord_y)
{
	if (env)
		return global_textureLod(image, st, max(mip_level, 0.0));
	if (mip_level >= 0.0)
	{
		float lod = mip_level;
		vec2 size = vec2(global_textureSize(image, 0));
		if (base_size.x > 0.0 && size.x > 0.0)
			lod += log2(size.x / base_size.x);
		lod += 0.5 * log2(max(abs(determinant(lin)), 1e-6));
		return global_textureLod(image, st, max(lod, 0.0));
	}
	return global_textureGrad(image, st, lin * tex_coord_x, lin * tex_coord_y);
}

/* Where the stage whose vec4s start at `o` reads its images, from `st0` (the
   surface's texture coordinates, or tcGen environment's): tcMods before the
   turb, the turb, the tcMods after it - and `lin`, the linear part of both
   matrices, for the footprint. */
void q3_stage_coords(uint o, uvec4 info, uint flags, vec2 st0, vec3 position, out vec2 st, out mat2 lin)
{
	vec4 m1 = light_buffer.q3_stages[o + 1u];     // a1 b1 c1 d1
	vec4 m12 = light_buffer.q3_stages[o + 2u];    // e1 f1 a2 b2
	vec4 m2 = light_buffer.q3_stages[o + 3u];     // c2 d2 e2 f2

	st = vec2(m1.x * st0.x + m1.y * st0.y + m12.x, m1.z * st0.x + m1.w * st0.y + m12.y);
	if ((flags & Q3P_STAGE_TURB) != 0u)
	{
		// RB_CalcTurbulentTexCoords: the warp follows the surface's position
		float amp = uintBitsToFloat(info.z);
		float now = uintBitsToFloat(info.w);
		st.x += sin(2.0 * M_PI * ((position.x + position.z) * (1.0 / 1024.0) + now)) * amp;
		st.y += sin(2.0 * M_PI * (position.y * (1.0 / 1024.0) + now)) * amp;
	}
	st = vec2(m12.z * st.x + m12.w * st.y + m2.z, m2.x * st.x + m2.y * st.y + m2.w);

	if ((flags & Q3P_STAGE_CLAMP) != 0u)
		st = clamp(st, vec2(0), vec2(1));

	lin = mat2(m12.z, m2.x, m12.w, m2.y) * mat2(m1.x, m1.z, m1.y, m1.w);
}

/* STAGE SETTINGS, for one drawn stage: its two settings vec4s at `o`, its own
   at `o_stage`, its blend, its colour s1 and the composite before and after
   it. Its texture coordinates are worked out again here rather than kept from
   the colour pass (registers, see q3_program_material). */
void q3_stage_props(uint o, uint o_stage, uvec4 info, uint flags, uint src_f, uint dst_f, vec4 s1, vec4 before, vec4 after,
	vec2 st0, vec3 position, bool env, float mip_level, vec2 base_size, vec2 tex_coord_x, vec2 tex_coord_y)
{
	if ((flags & Q3P_STAGE_LIGHTMAP) != 0u)
		return;

	uvec2 pi = floatBitsToUint(light_buffer.q3_stages[o].xy);
	uint pflags = pi.y >> 16;
	bool states = pflags != 0u;

	float t_cover, t_state;
	if (src_f == Q3P_BF_ZERO || src_f == Q3P_BF_DST_COLOR || src_f == Q3P_BF_ONE_MINUS_DST_COLOR)
	{
		// multiplied in
		t_cover = 0.0;
		t_state = 1.0;
	}
	else
	{
		t_cover = clamp(1.0 - dot(q3_blend_factor(dst_f, s1, before).rgb, vec3(1.0 / 3.0)), 0.0, 1.0);
		t_state = t_cover;
		if (t_state <= 0.0 && states)
		{
			// added on top
			float added = dot(s1.rgb * q3_blend_factor(src_f, s1, before).rgb, vec3(1.0 / 3.0));
			float total = dot(after.rgb, vec3(1.0 / 3.0));
			t_state = total > 1e-4 ? clamp(added / total, 0.0, 1.0) : 0.0;
		}
	}

	if (!states)
	{
		q3_prop_sum *= 1.0 - t_cover;
		q3_prop_w *= 1.0 - t_cover;
		q3_prop_normal *= 1.0 - t_cover;
		return;
	}

	vec4 pv = light_buffer.q3_stages[o + 1u];   // roughness_override, metalness_factor, specular_factor, bump_scale
	vec2 st;
	mat2 lin;
	q3_stage_coords(o_stage, info, flags, st0, position, st, lin);

	// as a material's: a roughness map with an override under it as a floor,
	// a metallic map scaled by the factor
	float rough = pv.x;
	if ((pflags & Q3P_PROP_ROUGH_MAP) != 0u)
	{
		float r = q3_stage_sample(pi.x & 0xffffu, st, lin, env, mip_level, base_size, tex_coord_x, tex_coord_y).r;
		rough = (pflags & Q3P_PROP_ROUGH) != 0u ? max(r, pv.x) : r;
	}
	float metal = pv.y;
	if ((pflags & Q3P_PROP_METAL_MAP) != 0u)
	{
		float m = q3_stage_sample(pi.x >> 16, st, lin, env, mip_level, base_size, tex_coord_x, tex_coord_y).r;
		metal = m * ((pflags & Q3P_PROP_METAL) != 0u ? pv.y : 1.0);
	}
	vec3 n = vec3(0, 0, 1);
	if ((pflags & Q3P_PROP_NORMAL_MAP) != 0u)
	{
		vec3 rgb = q3_stage_sample(pi.y & 0xffffu, st, lin, env, mip_level, base_size, tex_coord_x, tex_coord_y).rgb;
		vec3 m = vec3(rgb.xy * 2.0 - 1.0, rgb.z);     // as rgbToNormal
		// the map's slopes run along the stage's own axes, which its tcMods
		// turned: turned back onto the surface's (the turn only, not the scale)
		if (!env)
			m.xy = (transpose(lin) * m.xy) * inversesqrt(max(abs(determinant(lin)), 1e-6));
		if (dot(m, m) > 1e-8)
			n = normalize(mix(vec3(0, 0, 1), normalize(m), pv.w * global_ubo.pt_bump_scale));
	}

	bvec4 stated = bvec4((pflags & (Q3P_PROP_ROUGH | Q3P_PROP_ROUGH_MAP)) != 0u,
		(pflags & (Q3P_PROP_METAL | Q3P_PROP_METAL_MAP)) != 0u,
		(pflags & Q3P_PROP_SPEC) != 0u,
		(pflags & Q3P_PROP_NORMAL_MAP) != 0u);
	vec4 s = vec4(stated);
	vec4 t = mix(vec4(t_cover), vec4(t_state), stated);
	q3_prop_sum = q3_prop_sum * (1.0 - t) + vec4(clamp(rough, 0.0, 1.0), clamp(metal, 0.0, 1.0), pv.z, 0.0) * t * s;
	q3_prop_w = q3_prop_w * (1.0 - t) + t * s;
	q3_prop_normal = q3_prop_normal * (1.0 - t.w) + n * (t.w * s.w);
}

// Whether material_id's program has STAGE SETTINGS (Q3P_PROG_PROPS).
bool q3_program_has_props(uint material_id)
{
	uint prog = q3_program_offset(material_id);
	return prog != 0u && (floatBitsToUint(light_buffer.q3_stages[prog - 1u].x) & Q3P_PROG_PROPS) != 0u;
}

/* Composites material_id's program at a hit. False when the material has none
   (or it did not fit in this frame's table) - the caller then uses the
   material's own images. `normal` is the smooth surface normal, for tcGen
   environment; `mip_level` < 0 means sample with the gradients.

   `want_props`: a second pass for the STAGE SETTINGS (q3_prop_*), run by
   get_material only for a program that has them, where it uses them. Done in
   the first pass, the sums stayed live through the colour loop and the rest of
   get_material, and pushed the path tracers past 128 registers into local
   memory (2026-11-16: primary_rays 16 -> 64 bytes, indirect_lighting 96 -> 160)
   on every surface, for the few that have them. */
bool q3_program_material(
	uint material_id,
	vec3 position,
	vec2 tex_coord,
	vec2 tex_coord_x,
	vec2 tex_coord_y,
	float mip_level,
	vec3 normal,
	vec3 ray_direction,
	uint base_texture,
	bool want_props,
	out vec3 albedo,
	out vec3 emission,
	out float alpha)
{
	albedo = vec3(0);
	emission = vec3(0);
	alpha = 1.0;
	q3_shine_on = false;
	q3_shine = 0.0;
	q3_shine_metal = false;
	if (want_props)
	{
		q3_prop_sum = vec4(0);
		q3_prop_w = vec4(0);
		q3_prop_normal = vec3(0);
	}

	uint prog = q3_program_offset(material_id);
	if (prog == 0u)
		return false;

	uint base = prog - 1u;
	uint header = floatBitsToUint(light_buffer.q3_stages[base].x);
	uint num_stages = min(header & 0xffu, uint(Q3P_MAX_STAGES));
	bool translucent = (header & Q3P_PROG_TRANSLUCENT) != 0u;
	// STAGE SETTINGS: after the stages, Q3P_PROP_VEC4S each
	bool props = want_props && (header & Q3P_PROG_PROPS) != 0u;
	uint props_base = base + 1u + num_stages * uint(Q3P_STAGE_VEC4S);
	bool shine = (header & Q3P_PROG_SHINE) != 0u;
	bool shine_metal = (header & Q3P_PROG_SHINE_METAL) != 0u;
	bool glow = (header & Q3P_PROG_GLOW) != 0u;
	float share = 0.0;      // how much of c10 a reflection map made

	// RB_CalcEnvironmentTexCoords: the reflected view direction, in the
	// surface's own frame - which for the world is the world's
	vec3 viewer = -ray_direction;
	vec3 reflected = normal * (2.0 * dot(normal, viewer)) - viewer;
	vec2 env_st = vec2(0.5 + reflected.y * 0.5, 0.5 - reflected.z * 0.5);

	vec2 base_size = base_texture != 0u ? vec2(global_textureSize(base_texture, 0)) : vec2(0);

	vec4 c00 = vec4(0, 0, 0, 1);
	vec4 c10 = vec4(0, 0, 0, 1);
	vec4 c11 = vec4(1, 1, 1, 1);
	// Q3P_PROG_GLOW: C10 with the Q3P_STAGE_GLOW stages black - so C10 - CG is
	// what of them shows, fully lit
	vec4 cg = vec4(0, 0, 0, 1);

	for (uint i = 0u; i < num_stages; i++)
	{
		uint o = base + 1u + i * uint(Q3P_STAGE_VEC4S);
		uvec4 info = floatBitsToUint(light_buffer.q3_stages[o]);
		uint flags = info.x >> 16;
		bool env = (flags & Q3P_STAGE_ENV) != 0u;
		// a reflection map standing in for a shine (not a moving effect)
		bool env_shine = shine && env && (flags & Q3P_STAGE_ENV_EFFECT) == 0u;
		uint src_f = info.y & 0xfu;
		uint dst_f = (info.y >> 4) & 0xfu;
		vec4 color = light_buffer.q3_stages[o + 4u];

		// a reflection map added on top after the light: never drawn (the path
		// tracer reflects for real), but under pt_q3_env_shine it marks gloss
		if ((flags & Q3P_STAGE_HIGHLIGHT) != 0u)
		{
			if (env_shine)
				share = q3_shine_share(share, clamp(dot(color.rgb, vec3(1.0 / 3.0)), 0.0, 1.0),
					color, c10, src_f, dst_f);
			continue;
		}
		if ((flags & Q3P_STAGE_NO_IMAGE) != 0u)
			continue;

		vec4 tex = vec4(1);
		if ((flags & (Q3P_STAGE_LIGHTMAP | Q3P_STAGE_WHITE)) == 0u)
		{
			uint image = info.x & 0xffffu;
			vec2 st;
			mat2 lin;
			q3_stage_coords(o, info, flags, env ? env_st : tex_coord, position, st, lin);

			if (env_shine)
			{
				// the map's average colour, the same from every side - the scene
				// does the reflecting. A metal's is its reflectance: the map's
				// hue at a bright steel's luminance - when the surface is made of
				// it. One ADDED on top of a surface (dst ONE) stays what Quake III
				// added, or a near-black glint map like tinfx3 (0.005) came out
				// 0.6 grey and washed out the colour under it (Matt, 2026-11-17:
				// q3dm2's q1metal7_99spot grue went a white mirror)
				tex = global_textureLod(image, vec2(0.5), 16.0);
				if (shine_metal && dst_f != Q3P_BF_ONE)
				{
					float lum = dot(tex.rgb, vec3(0.2126, 0.7152, 0.0722));
					tex.rgb = lum > 1e-4 ? min(tex.rgb * (Q3_SHINE_METAL_F0 / lum), vec3(1)) : vec3(Q3_SHINE_METAL_F0);
				}
			}
			else
			{
				tex = q3_stage_sample(image, st, lin, env, mip_level, base_size, tex_coord_x, tex_coord_y);
			}
		}

		vec3 rgb = tex.rgb * color.rgb;
		float a = tex.a * color.a;

		uint alpha_func = (info.y >> 8) & 3u;
		if (alpha_func == 1u && !(a > 0.0))
			continue;
		if (alpha_func == 2u && !(a < 0.5))
			continue;
		if (alpha_func == 3u && !(a >= 0.5))
			continue;

		// the stage as it is with no light (s0) and with one unit of it (s1)
		vec4 s0 = vec4(rgb, a);
		vec4 s1 = vec4(rgb, a);
		if ((flags & (Q3P_STAGE_LIGHTMAP | Q3P_STAGE_LIT)) != 0u)
			s0.rgb = vec3(0);
		else if ((flags & Q3P_STAGE_ONE_MINUS_LIT) != 0u)
			s1.rgb = vec3(0);

		if (shine)
			share = q3_shine_share(share, env_shine ? clamp(dot(color.rgb, vec3(1.0 / 3.0)), 0.0, 1.0) : 0.0,
				s1, c10, src_f, dst_f);

		vec4 c10_before = c10;
		c00 =s0 * q3_blend_factor(src_f, s0, c00) + c00 * q3_blend_factor(dst_f, s0, c00);
		c10 = s1 * q3_blend_factor(src_f, s1, c10) + c10 * q3_blend_factor(dst_f, s1, c10);
		c11 = s1 * q3_blend_factor(src_f, s1, c11) + c11 * q3_blend_factor(dst_f, s1, c11);
		if (props)
			q3_stage_props(props_base + i * uint(Q3P_PROP_VEC4S), o, info, flags, src_f, dst_f, s1, c10_before, c10,
				env ? env_st : tex_coord, position, env, mip_level, base_size, tex_coord_x, tex_coord_y);
		if (glow)
		{
			vec4 sg = s1;
			if ((flags & Q3P_STAGE_GLOW) != 0u)
				sg.rgb = vec3(0);
			cg = sg * q3_blend_factor(src_f, sg, cg) + cg * q3_blend_factor(dst_f, sg, cg);
		}
	}

	// Q3P_PROG_SELF_LIT: a q3map_surfacelight shader whose every stage the light
	// multiplies (the medal and computer screens, lava) - C00 is black, and it
	// glows with its colour as fully lit instead (q3stages.c program_emits)
	emission = max((header & Q3P_PROG_SELF_LIT) != 0u ? c10.rgb : c00.rgb, vec3(0));
	// Q3P_PROG_GLOW: the stage the .mat's texture_emissive names glows with what
	// shows of it - the max, not the sum, as a stage that already emits is in C00
	if (glow)
		emission = max(emission, c10.rgb - cg.rgb);
	albedo = max(c10.rgb - c00.rgb, vec3(0));

	if (translucent)
	{
		vec3 through = clamp(c11.rgb - c10.rgb, vec3(0), vec3(1));
		alpha = clamp(1.0 - (through.r + through.g + through.b) * (1.0 / 3.0), 0.0, 1.0);
		// what is left of the surface reflects the light that falls on it; the
		// split in primary_rays weights it by alpha again
		if (alpha > 0.01)
			albedo /= alpha;
	}

	albedo = clamp(albedo, vec3(0), vec3(1));

	if (shine)
	{
		q3_shine_on = true;
		// Quake III's shiny panels show their reflection map at 30-50%: that much
		// already reads as a polished surface
		q3_shine = clamp(2.0 * share, 0.0, 1.0);
		q3_shine_metal = shine_metal;
	}
	return true;
}

#endif // Q3_STAGES_GLSL_
