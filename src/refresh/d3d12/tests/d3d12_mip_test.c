// Headless check of mipgen.c, the D3D12 renderer's CPU mip chains (textures.c), against
// what vkpt's linear vkCmdBlitImage chain and normalize_normal_map.comp give:
// - every sRGB code survives decode -> filter -> encode unchanged (a flat texture stays flat);
// - black and white average to sRGB 188 (linear 0.5), not 128;
// - an odd level takes the blit's scaled sample (5 -> 2: 0.25 / 0.75), a 3 -> 1 level its
//   centre texel; alpha and UNORM channels average linearly; R16 too;
// - normal maps renormalise with z kept as stored.
// No GPU. Build and run from a vcvars64 prompt, from the repository root:
//   cl /nologo /O2 src/refresh/d3d12/tests/d3d12_mip_test.c /Fe:build/tools/
//   build/tools/d3d12_mip_test.exe        (exit 0 = all checks pass)
#include <stdio.h>

#include "../mipgen.c"

static int failures;

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

int main(void)
{
    char what[160];

    // every code, flat 2x2 -> 1x1, sRGB colour and linear alpha
    {
        int bad = 0;
        for (int v = 0; v < 256; v++) {
            uint8_t src[16], dst[4];
            for (int i = 0; i < 16; i++)
                src[i] = (uint8_t)v;
            d3d12_mip_downsample(src, 2, 2, dst, 1, 1, 4, true);
            bad += dst[0] != v || dst[1] != v || dst[2] != v || dst[3] != v;
        }
        snprintf(what, sizeof(what), "flat sRGB texture keeps its code through a level (%d of 256 codes wrong)", bad);
        check(bad == 0, what);
    }

    // the encode's bucket table against counting the thresholds below each value, over
    // 2^20 values in [0, 1] and every threshold itself (a tie goes to the lower code)
    {
        int bad = 0;
        for (int i = 0; i <= (1 << 20) + 255; i++) {
            const float v = i <= (1 << 20) ? (float)i / (1 << 20) : srgb_threshold[i - (1 << 20) - 1];
            int want = 0;
            for (int k = 0; k < 255; k++)
                want += v > srgb_threshold[k];
            bad += linear_to_srgb8(v) != want;
        }
        snprintf(what, sizeof(what), "sRGB encode table exact (%d of %d values wrong)", bad, (1 << 20) + 256);
        check(bad == 0, what);
    }

    // black + white: linear 0.5 = sRGB 0.7354 = code 187.5 -> 188; alpha 0 + 255 linear -> 128
    {
        const uint8_t src[8] = { 0, 0, 0, 0, 255, 255, 255, 255 };
        uint8_t dst[4];
        d3d12_mip_downsample(src, 2, 1, dst, 1, 1, 4, true);
        snprintf(what, sizeof(what), "sRGB black + white -> %u %u %u (want 188), alpha %u (want 128)", dst[0], dst[1],
                 dst[2], dst[3]);
        check(dst[0] == 188 && dst[1] == 188 && dst[2] == 188 && dst[3] == 128, what);
        d3d12_mip_downsample(src, 2, 1, dst, 1, 1, 4, false);
        snprintf(what, sizeof(what), "UNORM black + white -> %u (want 128)", dst[0]);
        check(dst[0] == 128, what);
    }

    // a 5-texel row -> 2: centres at 0.75 and 3.25 of the row; UNORM 0 100 0 200 0 -> 75, 150
    {
        const uint8_t row[5] = { 0, 100, 0, 200, 0 };
        uint8_t src[20], dst[8];
        for (int i = 0; i < 5; i++)
            src[i * 4] = src[i * 4 + 1] = src[i * 4 + 2] = src[i * 4 + 3] = row[i];
        d3d12_mip_downsample(src, 5, 1, dst, 2, 1, 4, false);
        snprintf(what, sizeof(what), "odd row 5 -> 2: %u %u (want 75 150)", dst[0], dst[4]);
        check(dst[0] == 75 && dst[4] == 150 && dst[3] == 75 && dst[7] == 150, what);
    }

    // a 3x3 level -> 1x1: its centre texel only
    {
        uint8_t src[36], dst[4];
        for (int i = 0; i < 36; i++)
            src[i] = 10;
        for (int c = 0; c < 4; c++)
            src[4 * 4 + c] = 200;
        d3d12_mip_downsample(src, 3, 3, dst, 1, 1, 4, true);
        snprintf(what, sizeof(what), "3x3 -> 1x1 takes the centre: %u (want 200)", dst[0]);
        check(dst[0] == 200 && dst[3] == 200, what);
    }

    // R16 (16-bit grey): 1000 + 3000 -> 2000
    {
        const uint16_t src[4] = { 1000, 3000, 1000, 3000 };
        uint16_t dst[1];
        d3d12_mip_downsample((const uint8_t *)src, 2, 2, (uint8_t *)dst, 1, 1, 2, false);
        snprintf(what, sizeof(what), "R16 1000 + 3000 -> %u (want 2000)", dst[0]);
        check(dst[0] == 2000, what);
    }

    // a full chain of a non-square, non-power-of-two image runs to 1x1 with no garbage
    {
        enum { CW = 600, CH = 90 };
        static uint8_t a[CW * CH * 4], b[CW * CH * 4];
        for (int i = 0; i < CW * CH * 4; i++)
            a[i] = (uint8_t)(i * 7);
        int w = CW, h = CH, levels = 1;
        uint8_t *src = a, *dst = b;
        while (w > 1 || h > 1) {
            const int nw = w > 1 ? w >> 1 : w, nh = h > 1 ? h >> 1 : h;
            d3d12_mip_downsample(src, w, h, dst, nw, nh, 4, true);
            uint8_t *t = src; src = dst; dst = t;
            w = nw; h = nh; levels++;
        }
        snprintf(what, sizeof(what), "600x90 chain: %d levels (want 10, as 1 + log2(600)), last %u %u %u %u", levels,
                 src[0], src[1], src[2], src[3]);
        check(levels == 10, what);
    }

    // normals: (255, 128, 255) = (1, 0.0039, 1) -> (0.7071, 0.0028, 0.7071) -> 218 128 180; alpha kept
    {
        uint8_t n[8] = { 255, 128, 255, 77, 128, 128, 0, 9 };
        d3d12_mip_normalize_normals(n, 2);
        snprintf(what, sizeof(what), "normals -> %u %u %u a %u (want 218 128 180 a 77), %u %u %u a %u (want 218 218 0 a 9)",
                 n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
        check(n[0] == 218 && n[1] == 128 && n[2] == 180 && n[3] == 77 && n[4] == 218 && n[5] == 218 && n[6] == 0 &&
              n[7] == 9, what);
    }

    printf("\n%s (%d failed)\n", failures ? "FAILED" : "all checks pass", failures);
    return failures ? 1 : 0;
}
