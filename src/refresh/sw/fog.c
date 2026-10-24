/*
Copyright (C) 2026 Q2RTX rerelease port

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
// fog.c -- the rerelease's fog (sw_fog 1, 32-bit mode only)
//
// The original renderer had no fog. This is the rerelease's, as KEX draws it:
// an analytic tint per pixel from its depth and world height, after
// everything else - not the path tracer's lit medium. The formulas are
// q2repro's (src/refresh/shader.c), which reproduces KEX's quirks; the values
// are the map's own (CL_GetMapFogKEX), trigger_fog fades included.
//
//   distance fog   d = fog_density / 64 * depth; mix by 1 - exp(-d^2)
//   height fog     KEX's write_height_fog, "very broken" colour ramp and all
//   sky            mixed fog_sky_factor of the way to fog_color, nothing else
//
// It runs over the view buffer once the view is drawn, so translucent
// surfaces take the fog of what is behind them. The depth is the z buffer's,
// which is 1/z in 16 bits - too coarse for a smooth fog at a distance - so
// while fog is on the world's z spans also go into d_pfogz at full precision.
// A pixel a model or particle drew over has a different short z and falls
// back to the 16-bit value, which is plenty that close.

#include "sw.h"
#include <emmintrin.h>

#define FOG_TABLE_STEP  4       // world units a table entry
#define FOG_TABLE_SIZE  16384   // out to 65536 units

int32_t                 *d_pfogz;       // this frame's full precision 1/z, or NULL
static int32_t          *r_fogzbuffer;
static size_t           r_fogzbuffer_size;

static bool             r_fogactive;
static mapfog_kex_t     r_fog;

// by depth: the distance fog's blend, and 1 - exp(-heightfog_density * depth)
static struct {
    float   dist, height;
} fog_table[FOG_TABLE_SIZE];
static float    fog_column[MAXWIDTH];

// exp, to about 1e-5, without libm's cost
static inline float R_FastExp(float x)
{
    union { float f; int32_t i; } u;
    float   t, f, p;
    int     i;

    x = Q_clipf(x, -87.0f, 87.0f);
    t = x * 1.44269504f;
    i = (int)t;
    if (t < i)
        i--;
    f = t - i;
    p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f +
               f * (0.00961813f + f * 0.00133336f))));
    u.i = (i + 127) << 23;
    return u.f * p;
}

/*
=============
R_SetupFog

Before the view is drawn: whether this frame has fog, and if so the full
precision depth buffer for D_DrawZSpans.
=============
*/
void R_SetupFog(void)
{
    size_t  size;
    float   d;
    int     i;

    d_pfogz = NULL;
    r_fogactive = false;

    if (r_8bit || !sw_fog->integer || (r_newrefdef.rdflags & RDF_NOWORLDMODEL))
        return;
    if (!CL_GetMapFogKEX(&r_fog))
        return;

    size = (size_t)vid.width * vid.height;
    if (size > r_fogzbuffer_size) {
        Z_Free(r_fogzbuffer);
        r_fogzbuffer = R_Malloc(size * sizeof(r_fogzbuffer[0]));
        r_fogzbuffer_size = size;
    }

    d_pfogz = r_fogzbuffer;
    r_fogactive = true;

    for (i = 0; i < FOG_TABLE_SIZE; i++) {
        float z = (i + 0.5f) * FOG_TABLE_STEP;

        d = r_fog.density / 64 * z;
        fog_table[i].dist = 1.0f - R_FastExp(-(d * d));
        fog_table[i].height = 1.0f - R_FastExp(-(r_fog.hf_density * z));
    }
}

void R_FreeFog(void)
{
    Z_Freep((void **)&r_fogzbuffer);
    r_fogzbuffer_size = 0;
    d_pfogz = NULL;
}

/*
=============
R_DrawFog

Over the view buffer, BGRX, once everything is drawn: four pixels at a time
in SSE2, which every x86-64 has. The two fogs fold into one blend a pixel,
p * keep + add, as the distance fog's and then the height fog's mixes would
come out one after the other.
=============
*/
// the frame's fog, as the rows are drawn
typedef struct {
    vec3_t      fwd, right, up;
    float       color[3], start_color[3], end_color[3];
    float       sky, e_eye, falloff, cramp, cstart;
    bool        height_fog;
    int         v0, rows, jobs;
} fogpass_t;

// the same, four lanes wide, a channel to an entry in the pixel's BGR order
typedef struct {
    __m128  col[3], skyadd[3], hstart[3], hdelta[3];
    __m128  skykeep, right_z, falloff, neg_e_eye, org_z, cstart, cramp;
    __m128  row_z, row_len2;    // the row's
} fogsimd_t;

// R_FastExp, four at once
static inline __m128 R_FastExp4(__m128 x)
{
    __m128  t, fi, f, p;
    __m128i i;

    x = _mm_min_ps(_mm_max_ps(x, _mm_set1_ps(-87.0f)), _mm_set1_ps(87.0f));
    t = _mm_mul_ps(x, _mm_set1_ps(1.44269504f));
    i = _mm_cvttps_epi32(t);
    fi = _mm_cvtepi32_ps(i);
    // the truncation went up for negatives: floor it
    i = _mm_add_epi32(i, _mm_castps_si128(_mm_cmplt_ps(t, fi)));
    fi = _mm_cvtepi32_ps(i);
    f = _mm_sub_ps(t, fi);
    p = _mm_set1_ps(0.00133336f);
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(0.00961813f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(0.05550411f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(0.24022650f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(0.69314718f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(1.0f));
    return _mm_mul_ps(p, _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(i, _mm_set1_epi32(127)), 23)));
}

// m ? a : b
static inline __m128 R_Select4(__m128 m, __m128 a, __m128 b)
{
    return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
}

static inline __m128 R_Clamp4(__m128 x, __m128 lo, __m128 hi)
{
    return _mm_min_ps(_mm_max_ps(x, lo), hi);
}

// one channel of four BGRX pixels, blended
static inline __m128i R_MixChannel4(__m128i px, __m128i mask, int shift, __m128 keep, __m128 add)
{
    __m128  c = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(px, shift), _mm_set1_epi32(255)));

    c = R_Clamp4(_mm_add_ps(_mm_mul_ps(c, keep), add), _mm_setzero_ps(), _mm_set1_ps(255.0f));
    return _mm_and_si128(_mm_slli_epi32(_mm_cvttps_epi32(c), shift), mask);
}

static inline void R_Fog4(const fogpass_t *f, const fogsimd_t *s, byte *pdest,
                          const short *pz, const int32_t *pfz, const float *pcol)
{
    const __m128    one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
    __m128i px = _mm_loadu_si128((const __m128i *)pdest);
    __m128i zs = _mm_loadl_epi64((const __m128i *)pz);
    __m128i fz = _mm_loadu_si128((const __m128i *)pfz);
    __m128i izi, sky, ok, out;
    __m128  z, fd, keep, add0, add1, add2, skym;
    int32_t t[4];

    // the short z, 32 bits wide; 0 and below are the sky and the void
    zs = _mm_srai_epi32(_mm_unpacklo_epi16(zs, zs), 16);
    sky = _mm_cmpgt_epi32(_mm_set1_epi32(1), zs);

    // the full precision 1/z wherever no model or particle has drawn over it
    ok = _mm_and_si128(_mm_cmpeq_epi32(_mm_srai_epi32(fz, 16), zs),
                       _mm_cmpgt_epi32(fz, _mm_setzero_si128()));
    izi = _mm_or_si128(_mm_and_si128(ok, fz), _mm_andnot_si128(ok, _mm_slli_epi32(zs, 16)));
    // the sky's can be any depth at all, so long as it is one
    izi = _mm_or_si128(_mm_andnot_si128(sky, izi), _mm_and_si128(sky, _mm_set1_epi32(1 << 16)));
    z = _mm_div_ps(_mm_set1_ps(2147483648.0f), _mm_cvtepi32_ps(izi));

    _mm_storeu_si128((__m128i *)t, _mm_cvttps_epi32(R_Clamp4(
        _mm_mul_ps(z, _mm_set1_ps(1.0f / FOG_TABLE_STEP)), zero, _mm_set1_ps(FOG_TABLE_SIZE - 1))));

    fd = _mm_set_ps(fog_table[t[3]].dist, fog_table[t[2]].dist, fog_table[t[1]].dist, fog_table[t[0]].dist);
    keep = _mm_sub_ps(one, fd);
    add0 = _mm_mul_ps(s->col[0], fd);
    add1 = _mm_mul_ps(s->col[1], fd);
    add2 = _mm_mul_ps(s->col[2], fd);

    if (f->height_fog) {
        __m128  a = _mm_loadu_ps(pcol);
        __m128  ray_z = _mm_add_ps(s->row_z, _mm_mul_ps(s->right_z, a));
        __m128  len = _mm_sqrt_ps(_mm_add_ps(s->row_len2, _mm_mul_ps(a, a)));
        __m128  k = _mm_mul_ps(s->falloff, ray_z);
        __m128  kz = _mm_mul_ps(k, z);
        __m128  near_level, g, ext, frac, fh, hkeep, hw;

        // KEX's height fog is the integral of exp(-falloff * height) along
        // the ray, e_eye * len * (1 - exp(-kz)) / k for a pixel z deep. Its
        // own form takes two nearly equal exponentials from each other, and
        // came apart on rays running near level, where (1 - exp(-kz)) / k
        // is the series z * (1 - kz / 2 + kz^2 / 6) instead.
        near_level = _mm_cmplt_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), kz), _mm_set1_ps(0.03f));
        g = _mm_div_ps(_mm_sub_ps(one, R_FastExp4(_mm_sub_ps(zero, kz))), k);
        g = R_Select4(near_level, _mm_mul_ps(z, _mm_add_ps(one, _mm_mul_ps(kz,
                _mm_add_ps(_mm_set1_ps(-0.5f), _mm_mul_ps(kz, _mm_set1_ps(1.0f / 6)))))), g);
        ext = _mm_sub_ps(one, R_FastExp4(_mm_mul_ps(s->neg_e_eye, _mm_mul_ps(len, g))));
        ext = R_Clamp4(ext, zero, one);

        frac = _mm_add_ps(s->org_z, _mm_mul_ps(z, ray_z));
        frac = R_Clamp4(_mm_mul_ps(_mm_sub_ps(frac, s->cstart), s->cramp), zero, one);

        fh = _mm_set_ps(fog_table[t[3]].height, fog_table[t[2]].height,
                        fog_table[t[1]].height, fog_table[t[0]].height);
        fh = _mm_mul_ps(fh, ext);
        hkeep = _mm_sub_ps(one, fh);
        hw = _mm_mul_ps(ext, fh);

        keep = _mm_mul_ps(keep, hkeep);
        add0 = _mm_add_ps(_mm_mul_ps(add0, hkeep), _mm_mul_ps(_mm_add_ps(s->hstart[0], _mm_mul_ps(s->hdelta[0], frac)), hw));
        add1 = _mm_add_ps(_mm_mul_ps(add1, hkeep), _mm_mul_ps(_mm_add_ps(s->hstart[1], _mm_mul_ps(s->hdelta[1], frac)), hw));
        add2 = _mm_add_ps(_mm_mul_ps(add2, hkeep), _mm_mul_ps(_mm_add_ps(s->hstart[2], _mm_mul_ps(s->hdelta[2], frac)), hw));
    }

    // sky and void: the sky factor's share of the fog colour, nothing else
    skym = _mm_castsi128_ps(sky);
    keep = R_Select4(skym, s->skykeep, keep);
    add0 = R_Select4(skym, s->skyadd[0], add0);
    add1 = R_Select4(skym, s->skyadd[1], add1);
    add2 = R_Select4(skym, s->skyadd[2], add2);

    out = _mm_and_si128(px, _mm_set1_epi32(0xff000000));
    out = _mm_or_si128(out, R_MixChannel4(px, _mm_set1_epi32(0x0000ff), 0, keep, add0));
    out = _mm_or_si128(out, R_MixChannel4(px, _mm_set1_epi32(0x00ff00), 8, keep, add1));
    out = _mm_or_si128(out, R_MixChannel4(px, _mm_set1_epi32(0xff0000), 16, keep, add2));
    _mm_storeu_si128((__m128i *)pdest, out);
}

static void R_FogRows(void *arg, int index, int thread)
{
    const fogpass_t *f = arg;
    fogsimd_t   s;
    int         v0 = f->v0 + f->rows * index / f->jobs;
    int         v1 = f->v0 + f->rows * (index + 1) / f->jobs;
    int         x0 = r_refdef.vrect.x, x1 = r_refdef.vrectright;
    int         u, v, i, n;

    for (i = 0; i < 3; i++) {
        s.col[i] = _mm_set1_ps(f->color[2 - i]);
        s.skyadd[i] = _mm_set1_ps(f->color[2 - i] * f->sky);
        s.hstart[i] = _mm_set1_ps(f->start_color[2 - i]);
        s.hdelta[i] = _mm_set1_ps(f->end_color[2 - i] - f->start_color[2 - i]);
    }
    s.skykeep = _mm_set1_ps(1.0f - f->sky);
    s.right_z = _mm_set1_ps(f->right[2]);
    s.falloff = _mm_set1_ps(f->falloff);
    s.neg_e_eye = _mm_set1_ps(-f->e_eye);
    s.org_z = _mm_set1_ps(r_origin[2]);
    s.cstart = _mm_set1_ps(f->cstart);
    s.cramp = _mm_set1_ps(f->cramp);

    for (v = v0; v < v1; v++) {
        float   b = (r_refdef.ycenter - v) * r_refdef.yscaleinv;
        byte    *pdest = d_spantable[v] + x0 * VID_BYTES;
        short   *pz = d_zspantable[v];
        int32_t *pfz = d_pfogz + v * d_zwidth;

        s.row_z = _mm_set1_ps(f->fwd[2] + f->up[2] * b);
        s.row_len2 = _mm_set1_ps(1.0f + b * b);

        for (u = x0; u + 4 <= x1; u += 4, pdest += 4 * VID_BYTES)
            R_Fog4(f, &s, pdest, pz + u, pfz + u, fog_column + u);

        // the last few through a copy: the lanes past the row are sky
        if (u < x1) {
            uint32_t    px[4] = { 0 };
            short       zs[4] = { 0 };
            int32_t     fz[4] = { 0 };
            float       col[4] = { 0 };

            n = x1 - u;
            memcpy(px, pdest, n * VID_BYTES);
            memcpy(zs, pz + u, n * sizeof(zs[0]));
            memcpy(fz, pfz + u, n * sizeof(fz[0]));
            memcpy(col, fog_column + u, n * sizeof(col[0]));
            R_Fog4(f, &s, (byte *)px, zs, fz, col);
            memcpy(pdest, px, n * VID_BYTES);
        }
    }
}

void R_DrawFog(void)
{
    fogpass_t   f;
    float       eye;
    int         u, i;

    if (!r_fogactive)
        return;

    // the base vectors: brush models turn the live ones while they draw
    VectorCopy(base_vpn, f.fwd);
    VectorCopy(base_vright, f.right);
    VectorCopy(base_vup, f.up);

    for (i = 0; i < 3; i++) {
        f.color[i] = r_fog.color[i] * 255;
        f.start_color[i] = r_fog.hf_start_color[i] * 255;
        f.end_color[i] = r_fog.hf_end_color[i] * 255;
    }

    // no distance fog is a table of zeroes
    f.sky = Q_clipf(r_fog.sky_factor, 0, 1);
    f.height_fog = r_fog.hf_density > 0;

    f.falloff = r_fog.hf_falloff;
    eye = r_origin[2] - r_fog.hf_ref_z;
    f.e_eye = R_FastExp(-f.falloff * eye);
    f.cstart = r_fog.hf_color_start_z;
    f.cramp = r_fog.hf_color_end_z - r_fog.hf_color_start_z;
    f.cramp = f.cramp ? 1.0f / f.cramp : 0;

    // the view ray through each pixel is fwd + right * a + up * b, so a
    // pixel at depth z is at r_origin + z * ray
    for (u = r_refdef.vrect.x; u < r_refdef.vrectright; u++)
        fog_column[u] = (u - r_refdef.xcenter) * r_refdef.xscaleinv;

    f.v0 = r_refdef.vrect.y;
    f.rows = r_refdef.vrectbottom - r_refdef.vrect.y;
    f.jobs = R_RowJobs(f.rows);
    R_ParallelFor(f.jobs, R_FogRows, &f);
}
