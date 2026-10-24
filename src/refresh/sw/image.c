/*
Copyright (C) 1997-2001 Id Software, Inc.

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

#include "sw.h"

static byte gammatable[256];

#define NTX     256

static byte notexture_pixels[MIPSIZE(NTX * NTX) * TEX_BYTES];
static byte notexture_pixels8[MIPSIZE(NTX * NTX)];

/*
==============================================================================

PALETTE (8-bit mode)

==============================================================================
*/

byte        r_colormap[VID_GRADES * 256];
byte        r_alphamap[256 * 256];
uint32_t    r_palette[256];

// exact palette colours, so an 8-bit source keeps its own indices
#define PAL_HASH    1024

static int16_t  pal_hash[PAL_HASH];     // index + 1, 0 = empty
static uint32_t pal_rgb[256];           // 0xBBGGRR

// nearest index for each 5:6:5 colour, + 1; 0 = not looked up yet
static byte     pal_nearest[65536];
static bool     pal_nearest_valid[65536];

static inline unsigned PalHash(uint32_t rgb)
{
    return (rgb * 2654435761u) >> 22;
}

static int R_ExactIndex(uint32_t rgb)
{
    unsigned h = PalHash(rgb);

    while (pal_hash[h]) {
        if (pal_rgb[pal_hash[h] - 1] == rgb)
            return pal_hash[h] - 1;
        h = (h + 1) & (PAL_HASH - 1);
    }

    return -1;
}

// 255 is the transparent marker, never a colour
static byte R_NearestIndex(int r, int g, int b)
{
    int     i, best = 0, bestd = INT_MAX;

    for (i = 0; i < 255; i++) {
        int dr = r - (int)(pal_rgb[i] & 255);
        int dg = g - (int)((pal_rgb[i] >> 8) & 255);
        int db = b - (int)((pal_rgb[i] >> 16) & 255);
        int d = dr * dr + dg * dg + db * db;
        if (d < bestd) {
            bestd = d;
            best = i;
            if (!d)
                break;
        }
    }

    return best;
}

/*
================
R_Quantize

The palette index for a colour: its own index when it is a palette colour,
else the nearest to its 5:6:5 cell.
================
*/
byte R_Quantize(int r, int g, int b)
{
    unsigned    key;
    int         i;

    i = R_ExactIndex(r | (g << 8) | (b << 16));
    if (i >= 0 && i != 255)
        return i;

    key = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    if (!pal_nearest_valid[key]) {
        pal_nearest[key] = R_NearestIndex((r & 0xf8) | (r >> 5),
                                          (g & 0xfc) | (g >> 6),
                                          (b & 0xf8) | (b >> 5));
        pal_nearest_valid[key] = true;
    }

    return pal_nearest[key];
}

// RGBA to indices; with `transparent`, alpha under half becomes 255
static void R_Quantize8(byte *out, const byte *in, int count, bool transparent)
{
    int i;

    for (i = 0; i < count; i++, in += TEX_BYTES) {
        if (transparent && in[3] < 128)
            out[i] = 255;
        else
            out[i] = R_Quantize(in[0], in[1], in[2]);
    }
}

// what id's colormap holds, for when the file can't be had
static void R_BuildColormap(void)
{
    int     row, i, j;

    for (row = 0; row < VID_GRADES; row++) {
        float f = (float)(VID_GRADES - row) / (VID_GRADES / 2);
        for (i = 0; i < 256; i++) {
            if (i == 255) {
                r_colormap[row * 256 + i] = 255;
                continue;
            }
            r_colormap[row * 256 + i] = R_Quantize(
                min(255, (int)((pal_rgb[i] & 255) * f)),
                min(255, (int)(((pal_rgb[i] >> 8) & 255) * f)),
                min(255, (int)(((pal_rgb[i] >> 16) & 255) * f)));
        }
    }

    for (i = 0; i < 256; i++) {
        for (j = 0; j < 256; j++) {
            int k;
            byte c[3];
            for (k = 0; k < 3; k++)
                c[k] = (((pal_rgb[i] >> (k * 8)) & 255) * 2 + ((pal_rgb[j] >> (k * 8)) & 255)) / 3;
            r_alphamap[i * 256 + j] = R_Quantize(c[0], c[1], c[2]);
        }
    }
}

/*
================
R_InitPalette8

After IMG_GetPalette, before any image is loaded.
================
*/
void R_InitPalette8(void)
{
    byte    *pixels;
    int     i, ret;

    memset(pal_hash, 0, sizeof(pal_hash));
    memset(pal_nearest_valid, 0, sizeof(pal_nearest_valid));

    for (i = 0; i < 256; i++) {
        pal_rgb[i] = d_8to24table[i] & U32_RGB;
        if (i != 255 && R_ExactIndex(pal_rgb[i]) < 0) {
            unsigned h = PalHash(pal_rgb[i]);
            while (pal_hash[h])
                h = (h + 1) & (PAL_HASH - 1);
            pal_hash[h] = i + 1;
        }
    }

    pixels = R_Malloc(256 * 320);
    ret = IMG_LoadColormap(pixels);
    if (ret < 0) {
        Com_WPrintf("Couldn't load the colormap: %s; building one\n", Q_ErrorString(ret));
        R_BuildColormap();
    } else {
        memcpy(r_colormap, pixels, sizeof(r_colormap));
        memcpy(r_alphamap, pixels + sizeof(r_colormap), sizeof(r_alphamap));
    }
    Z_Free(pixels);

    R_SetPalette8(NULL);
}

/*
================
R_SetPalette8

The 8-bit view's output colours: the palette blended toward the screen
blend, then gamma corrected, the order R_CalcPalette did it in.
================
*/
void R_SetPalette8(const vec4_t blend)
{
    float   a = 0, one_minus;
    float   premult[3] = { 0, 0, 0 };
    int     i, k;

    if (blend && blend[3] > 0) {
        a = min(blend[3], 1.0f);
        for (k = 0; k < 3; k++)
            premult[k] = blend[k] * a * 255;
    }
    one_minus = 1.0f - a;

    for (i = 0; i < 256; i++) {
        int c[3];
        for (k = 0; k < 3; k++) {
            int v = premult[k] + one_minus * ((pal_rgb[i] >> (k * 8)) & 255);
            c[k] = gammatable[min(v, 255)];
        }
        r_palette[i] = (c[0] << 16) | (c[1] << 8) | c[2];
    }
}

/*
==============================================================================

IMAGES

==============================================================================
*/

/*
================
IMG_Unload
================
*/
void IMG_Unload_SW(image_t *image)
{
    if (image->sw_pixels[0] != notexture_pixels)
        Z_Free(image->sw_pixels[0]);
    if (image->sw_pixels8[0] != notexture_pixels8)
        Z_Free(image->sw_pixels8[0]);
    Z_Free(image->sw_glow[0]);
    memset(image->sw_pixels, 0, sizeof(image->sw_pixels));
    memset(image->sw_pixels8, 0, sizeof(image->sw_pixels8));
    memset(image->sw_glow, 0, sizeof(image->sw_glow));
}

static void R_LightScaleTexture(byte *in, int count)
{
    int     i;
    byte    *p;

    for (i = 0, p = in; i < count; i++, p += TEX_BYTES) {
        p[0] = gammatable[p[0]];
        p[1] = gammatable[p[1]];
        p[2] = gammatable[p[2]];
    }
}

/*
================
R_LoadGlowMap

sw_glowmaps: the glow map of a wall or skin, if it has one, at the size the
renderer keeps the image at. As the rerelease reads them (q2repro): a wall's
_glow takes its texels toward full light by its alpha and its colour is
ignored; a skin's is premultiplied by its alpha and added after lighting.
Q2RTX's _light maps have no useful alpha, so a wall's brightest channel
stands in for it. Taken before gamma, as the texture is, and given the same
gamma where it glows.
================
*/
static void R_LoadGlowMap(image_t *image)
{
    byte    *glow, *pic, *p;
    int     w, h, gw, gh, i, b, levels;
    bool    from_light, any = false;

    if (!sw_glowmaps->integer)
        return;
    if (image->type == IT_WALL) {
        if (image->flags & IF_TURBULENT)
            return;
        w = image->width;
        h = image->height;
    } else if (image->type == IT_SKIN) {
        w = image->upload_width;
        h = image->upload_height;
    } else {
        return;
    }

    glow = IMG_LoadGlowMap(image, &gw, &gh, &from_light);
    if (!glow)
        return;

    if (gw != w || gh != h) {
        pic = R_Malloc(w * h * TEX_BYTES);
        IMG_ResampleTexture(glow, gw, gh, pic, w, h);
        Z_Free(glow);
        glow = pic;
    }

    if (image->type == IT_SKIN) {
        for (i = 0, p = glow; i < w * h; i++, p += TEX_BYTES) {
            p[0] = p[0] * p[3] / 255;
            p[1] = p[1] * p[3] / 255;
            p[2] = p[2] * p[3] / 255;
            if (p[0] | p[1] | p[2]) {
                any = true;
                R_LightScaleTexture(p, 1);
            }
        }
        if (any)
            image->sw_glow[0] = glow;
        else
            Z_Free(glow);
        return;
    }

    // a wall: one byte a texel, through the texture's four mips, built from
    // the RGBA in place
    b = w * h;
    levels = 4;
    image->sw_glow[0] = R_Malloc(MIPSIZE(b));
    for (i = 1; i < levels; i++)
        image->sw_glow[i] = image->sw_glow[i - 1] + (b >> ((i - 1) * 2));

    for (i = 0; i < levels; i++) {
        int n = b >> (i * 2), j;

        if (i)
            IMG_MipMap(glow, glow, w >> (i - 1), h >> (i - 1));

        for (j = 0, p = glow; j < n; j++, p += TEX_BYTES) {
            int g = p[3];
            if (from_light)
                g = max(p[0], max(p[1], p[2])) * g / 255;
            image->sw_glow[i][j] = g;
            if (g)
                any = true;
        }
    }

    Z_Free(glow);

    if (!any) {
        Z_Free(image->sw_glow[0]);
        memset(image->sw_glow, 0, sizeof(image->sw_glow));
    }
}

/*
================
IMG_Load

Takes over `pic`, upload_width x upload_height RGBA.

Wall textures keep their nominal size - the one the map's texture axes are
in, which for a high resolution replacement is the original .wal's - with
three mip levels below it. The surface cache steps through them in blocks
of 16 texels at mip 0, so the size must be a multiple of 16; anything else
is resampled to the nearest that is. Warping liquids are always 64x64 and
skies 256x256, the sizes their drawers are written for.

Walls, skies, skins and sprites get palette indices too, for 8-bit mode,
made before gamma: gamma there is applied to the palette on output. A paletted
source keeps its own indices at full size.
================
*/
void IMG_Load_SW(image_t *image, byte *pic)
{
    int     i, c, b, levels, size;
    int     width, height;

    width = image->upload_width;
    height = image->upload_height;

    if (image->type == IT_WALL) {
        if (image->flags & IF_TURBULENT) {
            image->width = TURB_SIZE;
            image->height = TURB_SIZE;
        } else {
            image->width = Q_clip((image->width + 8) & ~15, 16, 1024);
            image->height = Q_clip((image->height + 8) & ~15, 16, 1024);
        }
    } else if (image->type == IT_SKY) {
        image->width = 256;
        image->height = 256;
    }

    if (image->type == IT_WALL || image->type == IT_SKY) {
        b = image->width * image->height;
        levels = image->type == IT_WALL ? 4 : 1;
        size = levels == 4 ? MIPSIZE(b) : b;

        image->sw_pixels[0] = R_Malloc(size * TEX_BYTES);
        image->sw_pixels8[0] = R_Malloc(size);
        for (i = 1; i < levels; i++) {
            image->sw_pixels[i] = image->sw_pixels[i - 1] + (b >> ((i - 1) * 2)) * TEX_BYTES;
            image->sw_pixels8[i] = image->sw_pixels8[i - 1] + (b >> ((i - 1) * 2));
        }

        if (width == image->width && height == image->height) {
            memcpy(image->sw_pixels[0], pic, width * height * TEX_BYTES);
        } else {
            IMG_ResampleTexture(pic, width, height, image->sw_pixels[0], image->width, image->height);
            image->upload_width = image->width;
            image->upload_height = image->height;
        }

        for (i = 1; i < levels; i++)
            IMG_MipMap(image->sw_pixels[i], image->sw_pixels[i - 1],
                       image->width >> (i - 1), image->height >> (i - 1));

        for (i = 0; i < levels; i++)
            R_Quantize8(image->sw_pixels8[i], image->sw_pixels[i], b >> (i * 2), false);

        R_LightScaleTexture(image->sw_pixels[0], size);

        Z_Free(pic);

        R_LoadGlowMap(image);
        return;
    }

    image->sw_pixels[0] = pic;
    c = width * height;

    if (!(image->flags & (IF_OPAQUE | IF_TRANSPARENT))) {
        for (i = 0; i < c; i++) {
            b = pic[i * TEX_BYTES + 3];
            if (b != 255) {
                image->flags |= IF_TRANSPARENT;
                break;
            }
        }
    }

    // sprites cut out at 255; skins never did
    if (image->type == IT_SKIN || image->type == IT_SPRITE) {
        image->sw_pixels8[0] = R_Malloc(c);
        R_Quantize8(image->sw_pixels8[0], pic, c, image->type == IT_SPRITE);
    }

    if (image->type == IT_SKIN) {
        R_LightScaleTexture(image->sw_pixels[0], c);
        R_LoadGlowMap(image);
    }
}

void R_BuildGammaTable(void)
{
    int     i, inf;
    float   g = vid_gamma->value;

    if (g == 1.0) {
        for (i = 0; i < 256; i++)
            gammatable[i] = i;
        return;
    }

    for (i = 0; i < 256; i++) {
        inf = 255 * pow((i + 0.5) / 255.5, g) + 0.5;
        gammatable[i] = Q_clip(inf, 0, 255);
    }
}

static void R_CreateNotexture(void)
{
    int         x, y, m;
    uint32_t    *p;
    byte        *p8, black8, white8;
    image_t     *ntx;

// create a simple checkerboard texture for the default
    ntx = R_NOTEXTURE;
    ntx->type = IT_WALL;
    ntx->flags = 0;
    ntx->width = ntx->height = NTX;
    ntx->upload_width = ntx->upload_height = NTX;
    ntx->sw_pixels[0] = notexture_pixels;
    ntx->sw_pixels[1] = ntx->sw_pixels[0] + NTX * NTX * TEX_BYTES;
    ntx->sw_pixels[2] = ntx->sw_pixels[1] + NTX * NTX * TEX_BYTES / 4;
    ntx->sw_pixels[3] = ntx->sw_pixels[2] + NTX * NTX * TEX_BYTES / 16;
    ntx->sw_pixels8[0] = notexture_pixels8;
    ntx->sw_pixels8[1] = ntx->sw_pixels8[0] + NTX * NTX;
    ntx->sw_pixels8[2] = ntx->sw_pixels8[1] + NTX * NTX / 4;
    ntx->sw_pixels8[3] = ntx->sw_pixels8[2] + NTX * NTX / 16;

    black8 = R_Quantize(0, 0, 0);
    white8 = R_Quantize(255, 255, 255);

    for (m = 0; m < 4; m++) {
        p = (uint32_t *)ntx->sw_pixels[m];
        p8 = ntx->sw_pixels8[m];
        for (y = 0; y < (NTX >> m); y++) {
            for (x = 0; x < (NTX >> m); x++) {
                if ((x ^ y) & (1 << (3 - m))) {
                    *p++ = U32_BLACK;
                    *p8++ = black8;
                } else {
                    *p++ = U32_WHITE;
                    *p8++ = white8;
                }
            }
        }
    }
}

/*
===============
R_InitImages
===============
*/
void R_InitImages(void)
{
    registration_sequence = 1;

    IMG_GetPalette();

    R_BuildGammaTable();

    R_InitPalette8();

    R_CreateNotexture();
}

/*
===============
R_ShutdownImages
===============
*/
void R_ShutdownImages(void)
{
    IMG_FreeAll();
}
