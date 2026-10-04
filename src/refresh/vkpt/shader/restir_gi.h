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

The reservoir is shaded on every frame, not only on frames whose first bounce went
diffuse: the diffuse/specular lobe choice (p = 1/2 on dielectrics, winner weighted
by 2) otherwise leaves the diffuse indirect term flipping between 2x and 0 from
frame to frame, which is most of the indirect noise and which reuse alone cannot
remove. The reservoir estimates the diffuse integral regardless of which frames
supplied its candidates, so it stands in at weight 1 every frame; the specular
lobe keeps its own 1/p estimate on the specular frames.

Per-pixel images, all double-buffered (A = this frame, B = last frame):

  PT_RESTIR_GI_POS     xyz = x_s, w = W (the reservoir's unbiased weight);
                       before restir_gi.rgen runs, w = the pixel's throughput
                       before the lobe choice (RGBE bits), every frame
  PT_RESTIR_GI_DATA    x = n_s (encode_normal), y = L_o (RGBE), z = M (float bits),
                       w = T0 (RGBE), this frame's throughput into the bounce, until
                       restir_gi.rgen replaces it with the sample's age in frames
  PT_RESTIR_GI_ORIGIN  xyz = the pixel's shading point x_v, w = its normal
                       (encode_normal, float bits). Needed because the first
                       bounce pass overwrites PT_SHADING_POSITION.

Before restir_gi.rgen runs, DATA.z is a candidate marker instead of M.

Target function: p_hat = luminance(L_o) * cos(theta_v). With cosine-weighted
candidates (p = cos / pi) a lone candidate gets W = pi / cos and shades exactly
what the path tracer would have added without ReSTIR.

An EMPTY reservoir - M > 0 but W = 0, because every sample it has seen was black -
is a valid history and is reused for its M (gi_is_valid). Treating it as garbage
reset dark pixels to M = 1 every frame, so a bounce that finally found light shaded
the pixel at M = 1, i.e. at full strength, and then decayed over m_clamp frames:
a one-frame glint per pixel per lucky bounce. With M kept, that bounce enters at
1 / (M + 1) of its value, which is the estimate it should give.

Pieces every production ReSTIR GI has and this one gained on 2026-10-04 (all in
restir_gi.rgen): a random per-frame permutation offset, a sample age limit
(pt_restir_gi_max_age), a boiling filter (pt_restir_gi_boiling) and final
visibility (pt_restir_gi_vis). Still missing: spatial reuse.

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
	float age;      // frames since the sample was drawn (0 = this frame's candidate)
};

GIReservoir gi_load(vec4 pos_w, uvec4 data)
{
	GIReservoir r;
	r.pos = pos_w.xyz;
	r.W = pos_w.w;
	r.normal = decode_normal(data.x);
	r.radiance = unpackRGBE(data.y);
	r.M = uintBitsToFloat(data.z);
	r.age = float(data.w);
	return r;
}

// Drop the sample but keep the history length: the pixel still knows how many
// bounces it has seen, so the next lit sample enters at its proper share.
void gi_drop(inout GIReservoir r)
{
	r.W = 0;
	r.radiance = vec3(0);
	r.age = 0;
}

// Garbage history - the first frame, a resize - must never be reused. An empty
// reservoir (M > 0, W = 0) is not garbage, see the header comment.
bool gi_is_valid(GIReservoir r)
{
	bool w_ok = global_ubo.pt_restir_gi_keep_empty != 0 ? r.W >= 0 : r.W > 0;
	return r.M > 0 && r.M < 1e6 && w_ok && !isinf(r.W) && !isnan(r.W)
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
