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
ReSTIR GI (pt_restir_gi) - Ouyang et al., "ReSTIR GI: Path Resampling for
Real-Time Path Tracing", HPG 2021. Temporal reuse only for now.

The first DIFFUSE bounce of every pixel becomes a candidate sample instead of
lighting the pixel directly: the point its ray hit (x_s, n_s) and the light
leaving that point towards the pixel, L_o - everything the path tracer puts
behind that hit, including the second bounce and the SHaRC cache. Each pixel
keeps a reservoir of one such sample across frames; restir_gi.rgen merges this
frame's candidate with last frame's reservoir and lights the pixel with the
winner. A good sample - one that found the lit wall - survives for several
frames instead of being thrown away, which is where the noise goes.

Per-pixel images, all double-buffered (A = this frame, B = last frame):

  PT_RESTIR_GI_POS     xyz = x_s, w = W (the reservoir's unbiased weight)
  PT_RESTIR_GI_DATA    x = n_s (encode_normal), y = L_o (RGBE), z = M (float bits),
                       w = T0 (RGBE), this frame's throughput into the bounce
  PT_RESTIR_GI_ORIGIN  xyz = the pixel's shading point x_v, w = its normal
                       (encode_normal, float bits). Needed because the first
                       bounce pass overwrites PT_SHADING_POSITION.

Before restir_gi.rgen runs, DATA.z is a candidate marker instead of M.

Target function: p_hat = luminance(L_o) * cos(theta_v). With cosine-weighted
candidates (p = cos / pi) a lone candidate gets W = pi / cos and shades exactly
what the path tracer would have added without ReSTIR.

Never in accumulation (photo) mode: sample reuse is biased there, and that mode
is the unbiased reference.
*/

#ifndef RESTIR_GI_H_
#define RESTIR_GI_H_

#define RESTIR_GI_CAND_DIFFUSE   0.0  // a fresh diffuse candidate, M not yet assigned
#define RESTIR_GI_CAND_SPECULAR -2.0  // this frame's first bounce went specular: reuse only
#define RESTIR_GI_CAND_INVALID  -1.0  // nothing to light (sky, invalid surface, lava)

bool restir_gi_enabled()
{
	return global_ubo.pt_restir_gi != 0
	    && global_ubo.temporal_blend_factor == 0
	    && global_ubo.pt_num_bounce_rays >= 1;
}

struct GIReservoir
{
	vec3 pos;       // x_s
	vec3 normal;    // n_s, facing the point that sampled it
	vec3 radiance;  // L_o, towards the point that sampled it
	float W;
	float M;
};

GIReservoir gi_load(vec4 pos_w, uvec4 data)
{
	GIReservoir r;
	r.pos = pos_w.xyz;
	r.W = pos_w.w;
	r.normal = decode_normal(data.x);
	r.radiance = unpackRGBE(data.y);
	r.M = uintBitsToFloat(data.z);
	return r;
}

// Garbage history - the first frame, a resize - must never be reused.
bool gi_is_valid(GIReservoir r)
{
	return r.M > 0 && r.M < 1e6 && r.W > 0 && !isinf(r.W) && !isnan(r.W)
	    && !any(isnan(r.pos)) && !any(isinf(r.pos))
	    && !any(isnan(r.radiance)) && !any(isinf(r.radiance));
}

// The target function for a sample seen from shading point x_v with normal n_v.
// Zero when the sample is behind the surface or faces away from it.
float gi_p_hat(vec3 radiance, vec3 sample_pos, vec3 sample_normal, vec3 x_v, vec3 n_v)
{
	vec3 d = sample_pos - x_v;
	float len = length(d);
	if (len <= 0)
		return 0;
	d /= len;
	float cos_v = dot(n_v, d);
	float cos_s = dot(sample_normal, -d);
	if (cos_v <= 0 || cos_s <= 0)
		return 0;
	return luminance(radiance) * cos_v;
}

/* Reconnection shift Jacobian (ReSTIR GI eq. 11): moving the sample from the point
   that found it (x_r) to this one (x_q) changes the solid angle it subtends. */
float gi_jacobian(vec3 sample_pos, vec3 sample_normal, vec3 x_q, vec3 x_r)
{
	vec3 dq = x_q - sample_pos;
	vec3 dr = x_r - sample_pos;
	float lq2 = dot(dq, dq);
	float lr2 = dot(dr, dr);
	if (lq2 <= 0 || lr2 <= 0)
		return 0;
	float cos_q = abs(dot(sample_normal, dq)) * inversesqrt(lq2);
	float cos_r = abs(dot(sample_normal, dr)) * inversesqrt(lr2);
	if (cos_r <= 1e-4)
		return 0;
	return (cos_q / cos_r) * (lr2 / lq2);
}

#endif // RESTIR_GI_H_
