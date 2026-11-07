/*
MIP CHAINS ON THE CPU, as vkpt makes them on the GPU (vkpt/textures.c, phase 4).

vkpt fills a texture's chain level by level with vkCmdBlitImage and a linear filter.
d3d12_mip_downsample reproduces that blit:
- Each texel of the level below is a bilinear sample of the level above, taken at the
  texel's own centre mapped into that level, clamped to the edge. An even-sized level
  gives the 2x2 average; an odd-sized one (a 5-texel row becomes 2) gives the scaled
  sample, 0.25 and 0.75 of its two neighbours.
- An sRGB image is filtered on linear values and stored back as sRGB, rounded to the
  nearest code, as a blit between sRGB images does. Alpha stays linear.
- Each level comes from the level above as it was stored (8 or 16 bits), because that
  is what the blit reads.
The GPU's filter weights are fixed point (8 bits on NVIDIA), so an odd ratio such as
7 -> 3 can land one code apart from these float ones. Even sizes, the usual case,
average exactly.

POM height maps keep vkpt's own max-filtered chain (vkpt_downsample_max, called from
textures.c). Normal maps are renormalised first (d3d12_mip_normalize_normals, the
same arithmetic as normalize_normal_map.comp).

Done here, at upload, rather than on the GPU: that needs no UAV-flagged textures and
no per-level descriptors. A map's textures already pay a PNG/TGA decode on the CPU,
which is several times this cost.
*/

#include "mipgen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* sRGB through tables. Decoding is one lookup. Encoding finds the code whose interval
   holds a linear value: the thresholds between codes lie at least 0.0003 apart (near
   black, where sRGB is steepest), more than a 1/4096 bucket is wide, so a bucket holds
   at most one. srgb_bucket gives the code at a bucket's start, and one compare against
   the next threshold settles it - exact, and several times faster than a search. */
#define SRGB_BUCKETS    4096

static float   srgb_linear[256];                // code -> linear
static float   srgb_threshold[256];             // linear halfway between codes k and k + 1 (255: none)
static uint8_t srgb_bucket[SRGB_BUCKETS];       // the code at the start of each bucket
static bool    tables_made;

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void make_tables(void)
{
    for (int i = 0; i < 256; i++)
        srgb_linear[i] = srgb_to_linear((float)i / 255.f);
    for (int k = 0; k < 255; k++)
        srgb_threshold[k] = srgb_to_linear(((float)k + 0.5f) / 255.f);
    srgb_threshold[255] = 2.f;
    int code = 0;
    for (int i = 0; i < SRGB_BUCKETS; i++) {
        const float start = (float)i / SRGB_BUCKETS;
        while (code < 255 && srgb_threshold[code] < start)
            code++;
        srgb_bucket[i] = (uint8_t)code;
    }
    tables_made = true;
}

// the code whose interval holds `v`: how many halfway points lie below it
static uint8_t linear_to_srgb8(float v)
{
    int i = (int)(v * SRGB_BUCKETS);
    i = i < 0 ? 0 : i > SRGB_BUCKETS - 1 ? SRGB_BUCKETS - 1 : i;
    const int code = srgb_bucket[i];
    return (uint8_t)(code + (v > srgb_threshold[code]));
}

static uint8_t unorm8(float v)
{
    v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    return (uint8_t)(v * 255.f + 0.5f);
}

static uint16_t unorm16(float v)
{
    v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    return (uint16_t)(v * 65535.f + 0.5f);
}

typedef struct {
    int     i0, i1;     // the two texels, clamped to the level
    float   f;          // i1's weight
} tap_t;

// where each of `n_dst` texel centres lands among `n_src`
static void make_taps(tap_t *t, int n_dst, int n_src)
{
    const float scale = (float)n_src / (float)n_dst;
    for (int x = 0; x < n_dst; x++) {
        const float u = ((float)x + 0.5f) * scale - 0.5f;
        const int i0 = (int)floorf(u);
        t[x].f = u - (float)i0;
        t[x].i0 = i0 < 0 ? 0 : i0 > n_src - 1 ? n_src - 1 : i0;
        t[x].i1 = i0 + 1 < 0 ? 0 : i0 + 1 > n_src - 1 ? n_src - 1 : i0 + 1;
    }
}

static float bilerp(float a, float b, float c, float d, float fx, float fy)
{
    const float top = a + (b - a) * fx;
    const float bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
}

void d3d12_mip_downsample(const uint8_t *src, int wd, int ht, uint8_t *dst, int nwd, int nht, int bpp, bool srgb)
{
    if (!tables_made)
        make_tables();

    tap_t *tx = malloc(sizeof(tap_t) * ((size_t)nwd + (size_t)nht));
    if (!tx)
        return;
    tap_t *ty = tx + nwd;
    make_taps(tx, nwd, wd);
    make_taps(ty, nht, ht);

    if (bpp == 2) {
        const uint16_t *s = (const uint16_t *)src;
        uint16_t *d = (uint16_t *)dst;
        const float k = 1.f / 65535.f;
        for (int y = 0; y < nht; y++) {
            const uint16_t *r0 = s + (size_t)ty[y].i0 * wd, *r1 = s + (size_t)ty[y].i1 * wd;
            for (int x = 0; x < nwd; x++) {
                const int a = tx[x].i0, b = tx[x].i1;
                d[(size_t)y * nwd + x] = unorm16(bilerp(r0[a] * k, r0[b] * k, r1[a] * k, r1[b] * k, tx[x].f, ty[y].f));
            }
        }
        free(tx);
        return;
    }

    const float k = 1.f / 255.f;
    for (int y = 0; y < nht; y++) {
        const uint8_t *r0 = src + (size_t)ty[y].i0 * wd * 4, *r1 = src + (size_t)ty[y].i1 * wd * 4;
        uint8_t *out = dst + (size_t)y * nwd * 4;
        for (int x = 0; x < nwd; x++) {
            const int a = tx[x].i0 * 4, b = tx[x].i1 * 4;
            for (int c = 0; c < 4; c++) {
                if (srgb && c < 3) {
                    const float v = bilerp(srgb_linear[r0[a + c]], srgb_linear[r0[b + c]], srgb_linear[r1[a + c]],
                                           srgb_linear[r1[b + c]], tx[x].f, ty[y].f);
                    out[x * 4 + c] = linear_to_srgb8(v);
                } else {
                    out[x * 4 + c] = unorm8(bilerp(r0[a + c] * k, r0[b + c] * k, r1[a + c] * k, r1[b + c] * k,
                                                   tx[x].f, ty[y].f));
                }
            }
        }
    }
    free(tx);
}

/* normalize_normal_map.comp's arithmetic: x and y unpacked to [-1, 1], z kept in [0, 1]
   as stored, the vector normalised (a zero one becomes straight up), x and y packed
   again; alpha untouched. imageStore's rounding to the nearest code. */
void d3d12_mip_normalize_normals(uint8_t *rgba, size_t texels)
{
    for (size_t i = 0; i < texels; i++) {
        uint8_t *p = rgba + i * 4;
        float x = (float)p[0] / 255.f * 2.f - 1.f;
        float y = (float)p[1] / 255.f * 2.f - 1.f;
        float z = (float)p[2] / 255.f;
        const float len = sqrtf(x * x + y * y + z * z);
        if (len == 0.f) {
            x = 0.f;
            y = 0.f;
            z = 1.f;
        } else {
            x /= len;
            y /= len;
            z /= len;
        }
        p[0] = unorm8(x * 0.5f + 0.5f);
        p[1] = unorm8(y * 0.5f + 0.5f);
        p[2] = unorm8(z);
    }
}
