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

// draw.c -- 2D drawing
//
// The same API as the GL renderer's draw.c: coordinates are in virtual
// units, R_SetScale's `scale` of them to a pixel, so the HUD and console
// come out the same size at any resolution. Pictures are sampled nearest,
// the way the original software renderer drew them.

#include "sw.h"

typedef struct {
    color_t     colors[2];  // normal and alt text colour
    float       scale;      // virtual units per pixel
    bool        clipped;
    clipRect_t  clip;       // in pixels, when clipped
} drawStatic_t;

static drawStatic_t draw;

// texel byte offset of each destination column / row of the current draw
static int  draw_cols[MAXWIDTH];

// textures are RGBA, the window BGRX
static inline uint32_t RGBA_BGRX(uint32_t c)
{
    return ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
}

// d = d + (s * c - d) * alpha, alpha = s.a * c.a
static inline void BlendTexel(byte *d, const byte *s, const byte *c)
{
    int a = (s[3] * (c[3] + 1)) >> 8;

    if (!a)
        return;

    a += a >> 7;    // 0..256
    d[0] = (d[0] * (256 - a) + ((s[2] * (c[2] + 1)) >> 8) * a) >> 8;
    d[1] = (d[1] * (256 - a) + ((s[1] * (c[1] + 1)) >> 8) * a) >> 8;
    d[2] = (d[2] * (256 - a) + ((s[0] * (c[0] + 1)) >> 8) * a) >> 8;
}

static inline void BlendColor(byte *d, const byte *c, int a)
{
    d[0] = (d[0] * (256 - a) + c[2] * a) >> 8;
    d[1] = (d[1] * (256 - a) + c[1] * a) >> 8;
    d[2] = (d[2] * (256 - a) + c[0] * a) >> 8;
}

/*
=============
R_PixelBounds

The pixels whose centres fall in [x0, x1) x [y0, y1), given in pixels, cut
to the window and the clip rectangle. False if there are none.
=============
*/
static bool R_PixelBounds(float x0, float y0, float x1, float y1, int *rect)
{
    int left, top, right, bottom;

    if (!vid.buffer)
        return false;

    left = (int)ceilf(x0 - 0.5f);
    top = (int)ceilf(y0 - 0.5f);
    right = (int)ceilf(x1 - 0.5f);
    bottom = (int)ceilf(y1 - 0.5f);

    left = max(left, 0);
    top = max(top, 0);
    right = min(right, vid.width);
    bottom = min(bottom, vid.height);

    if (draw.clipped) {
        left = max(left, draw.clip.left);
        top = max(top, draw.clip.top);
        right = min(right, draw.clip.right);
        bottom = min(bottom, draw.clip.bottom);
    }

    if (right <= left || bottom <= top)
        return false;

    rect[0] = left;
    rect[1] = top;
    rect[2] = right;
    rect[3] = bottom;
    return true;
}

// texel index for texture coordinate `s` across `size` texels
static inline int R_Texel(float s, int size, bool tile)
{
    int i = (int)floorf(s * size);

    if (tile) {
        i %= size;
        if (i < 0)
            i += size;
        return i;
    }

    return Q_clip(i, 0, size - 1);
}

/*
=============
R_DrawImage

Draws the s1,t1 - s2,t2 part of `image` (texture coordinates, 0..1 across
it) into the virtual rectangle x, y, w, h, modulated by `color`. With
`tile` the coordinates wrap.
=============
*/
static void R_DrawImage(float x, float y, float w, float h,
                        float s1, float t1, float s2, float t2,
                        uint32_t color, const image_t *image, bool tile)
{
    float       inv = 1.0f / draw.scale;
    float       x0 = x * inv, y0 = y * inv;
    float       x1 = (x + w) * inv, y1 = (y + h) * inv;
    int         rect[4], px, py, tw, th, count;
    const byte  *pixels, *src;
    byte        *dst;
    color_t     c;
    bool        plain;

    if (!image || !(pixels = image->sw_pixels[0]))
        return;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (!R_PixelBounds(x0, y0, x1, y1, rect))
        return;

    tw = image->upload_width;
    th = image->upload_height;
    if (tw < 1 || th < 1)
        return;

    count = rect[2] - rect[0];
    for (px = 0; px < count; px++) {
        float f = (rect[0] + px + 0.5f - x0) / (x1 - x0);
        draw_cols[px] = R_Texel(s1 + (s2 - s1) * f, tw, tile) * TEX_BYTES;
    }

    c.u32 = color;
    plain = c.u32 == U32_WHITE;

    for (py = rect[1]; py < rect[3]; py++) {
        float f = (py + 0.5f - y0) / (y1 - y0);
        int row = R_Texel(t1 + (t2 - t1) * f, th, tile);

        src = pixels + row * tw * TEX_BYTES;
        dst = vid.buffer + py * vid.rowbytes + rect[0] * VID_BYTES;

        if (plain && !(image->flags & IF_TRANSPARENT)) {
            uint32_t *d = (uint32_t *)dst;
            for (px = 0; px < count; px++)
                d[px] = RGBA_BGRX(*(const uint32_t *)(src + draw_cols[px]));
        } else if (plain) {
            for (px = 0; px < count; px++, dst += VID_BYTES) {
                const byte *s = src + draw_cols[px];
                if (s[3] == 255)
                    *(uint32_t *)dst = RGBA_BGRX(*(const uint32_t *)s);
                else if (s[3])
                    BlendTexel(dst, s, c.u8);
            }
        } else {
            for (px = 0; px < count; px++, dst += VID_BYTES)
                BlendTexel(dst, src + draw_cols[px], c.u8);
        }
    }
}

/*
=============
R_FillRect
=============
*/
static void R_FillRect(float x, float y, float w, float h, uint32_t color)
{
    float       inv = 1.0f / draw.scale;
    int         rect[4], px, py, a;
    uint32_t    *d, fill;
    color_t     c;

    if (!R_PixelBounds(x * inv, y * inv, (x + w) * inv, (y + h) * inv, rect))
        return;

    c.u32 = color;
    a = c.u8[3] + (c.u8[3] >> 7);
    if (!a)
        return;

    fill = RGBA_BGRX(color);

    for (py = rect[1]; py < rect[3]; py++) {
        d = (uint32_t *)(vid.buffer + py * vid.rowbytes) + rect[0];
        if (a == 256) {
            for (px = rect[0]; px < rect[2]; px++)
                *d++ = fill;
        } else {
            for (px = rect[0]; px < rect[2]; px++, d++)
                BlendColor((byte *)d, c.u8, a);
        }
    }
}

/*
=============
R_BlendScreen

The full screen tint (damage, pickups, underwater) over the 3D view.
=============
*/
typedef struct {
    int         rect[4], a, jobs;
    color_t     c;
} blendpass_t;

static void R_BlendRows(void *arg, int index, int thread)
{
    const blendpass_t   *p = arg;
    int                 rows = p->rect[3] - p->rect[1];
    int                 y0 = p->rect[1] + rows * index / p->jobs;
    int                 y1 = p->rect[1] + rows * (index + 1) / p->jobs;
    int                 px, py;
    byte                *d;

    for (py = y0; py < y1; py++) {
        d = vid.buffer + py * vid.rowbytes + p->rect[0] * VID_BYTES;
        for (px = p->rect[0]; px < p->rect[2]; px++, d += VID_BYTES)
            BlendColor(d, p->c.u8, p->a);
    }
}

void R_BlendScreen(const vec4_t blend)
{
    blendpass_t p;
    int         a;
    color_t     c;

    a = Q_clip(blend[3] * 256, 0, 256);
    if (!a)
        return;

    c.u8[0] = Q_clip(blend[0] * 255, 0, 255);
    c.u8[1] = Q_clip(blend[1] * 255, 0, 255);
    c.u8[2] = Q_clip(blend[2] * 255, 0, 255);
    c.u8[3] = 255;

    if (!R_PixelBounds(r_newrefdef.x, r_newrefdef.y,
                       r_newrefdef.x + r_newrefdef.width,
                       r_newrefdef.y + r_newrefdef.height, p.rect))
        return;

    p.a = a;
    p.c = c;
    p.jobs = R_RowJobs(p.rect[3] - p.rect[1]);
    R_ParallelFor(p.jobs, R_BlendRows, &p);
}

void R_InitDraw(void)
{
    memset(&draw, 0, sizeof(draw));
    draw.colors[0].u32 = U32_WHITE;
    draw.colors[1].u32 = U32_WHITE;
    draw.scale = 1;
}

void R_ClearColor_SW(void)
{
    draw.colors[0].u32 = U32_WHITE;
    draw.colors[1].u32 = U32_WHITE;
}

void R_SetAlpha_SW(float alpha)
{
    draw.colors[0].u8[3] =
        draw.colors[1].u8[3] = Q_clip(alpha * 255, 0, 255);
}

void R_SetAlphaScale_SW(float alpha)
{
    // nop - only used by the RTX renderer
}

void R_SetColor_SW(uint32_t color)
{
    draw.colors[0].u32 = color;
    draw.colors[1].u8[3] = draw.colors[0].u8[3];
}

void R_SetClipRect_SW(const clipRect_t *clip)
{
    float scale;

    draw.clipped = false;
    if (!clip)
        return;

    scale = 1 / draw.scale;

    draw.clip.left = max(clip->left * scale, 0);
    draw.clip.top = max(clip->top * scale, 0);
    draw.clip.right = min(clip->right * scale, r_config.width);
    draw.clip.bottom = min(clip->bottom * scale, r_config.height);

    if (draw.clip.right < draw.clip.left || draw.clip.bottom < draw.clip.top)
        return;

    draw.clipped = true;
}

void R_SetScale_SW(float scale)
{
    draw.scale = scale > 0 ? scale : 1;
}

void R_DrawStretchPic_SW(int x, int y, int w, int h, qhandle_t pic)
{
    R_DrawImage(x, y, w, h, 0, 0, 1, 1, draw.colors[0].u32, IMG_ForHandle(pic), false);
}

void R_DrawPic_SW(int x, int y, qhandle_t pic)
{
    image_t *image = IMG_ForHandle(pic);

    R_DrawImage(x, y, image->width, image->height, 0, 0, 1, 1,
                draw.colors[0].u32, image, false);
}

// A pic w x h centred on (cx, cy), turned clockwise by `angle` radians.
void R_DrawRotatedPic_SW(float cx, float cy, float w, float h, float angle, qhandle_t pic)
{
    image_t     *image = IMG_ForHandle(pic);
    float       inv = 1.0f / draw.scale;
    float       c = cosf(angle), s = sinf(angle);
    float       ex, ey;
    int         rect[4], px, py, tw, th;
    const byte  *pixels;
    byte        *dst;

    if (!(pixels = image->sw_pixels[0]))
        return;

    cx *= inv; cy *= inv; w *= inv; h *= inv;
    if (w <= 0 || h <= 0)
        return;

    // half extents of the turned rectangle's bounding box
    ex = (fabsf(w * c) + fabsf(h * s)) * 0.5f;
    ey = (fabsf(w * s) + fabsf(h * c)) * 0.5f;

    if (!R_PixelBounds(cx - ex, cy - ey, cx + ex, cy + ey, rect))
        return;

    tw = image->upload_width;
    th = image->upload_height;

    for (py = rect[1]; py < rect[3]; py++) {
        float dy = py + 0.5f - cy;

        dst = vid.buffer + py * vid.rowbytes + rect[0] * VID_BYTES;
        for (px = rect[0]; px < rect[2]; px++, dst += VID_BYTES) {
            float dx = px + 0.5f - cx;
            // along the width edge (c, s) and the height edge (-s, c)
            float fs = (dx * c + dy * s) / w + 0.5f;
            float ft = (dy * c - dx * s) / h + 0.5f;
            int u, v;

            if (fs < 0 || fs >= 1 || ft < 0 || ft >= 1)
                continue;

            u = min((int)(fs * tw), tw - 1);
            v = min((int)(ft * th), th - 1);
            BlendTexel(dst, pixels + (v * tw + u) * TEX_BYTES, draw.colors[0].u8);
        }
    }
}

#define DIV64 (1.0f / 64.0f)

void R_TileClear_SW(int x, int y, int w, int h, qhandle_t pic)
{
    R_DrawImage(x, y, w, h, x * DIV64, y * DIV64,
                (x + w) * DIV64, (y + h) * DIV64, U32_WHITE, IMG_ForHandle(pic), true);
}

void R_DrawFill8_SW(int x, int y, int w, int h, int c)
{
    if (!w || !h)
        return;
    R_FillRect(x, y, w, h, d_8to24table[c & 0xff]);
}

void R_DrawFill32_SW(int x, int y, int w, int h, uint32_t color)
{
    if (!w || !h)
        return;
    R_FillRect(x, y, w, h, color);
}

static inline void draw_char(int x, int y, int flags, int c, const image_t *image)
{
    float s, t;

    if ((c & 127) == 32) {
        return;
    }

    if (flags & UI_ALTCOLOR) {
        c |= 0x80;
    }
    if (flags & UI_XORCOLOR) {
        c ^= 0x80;
    }

    s = (c & 15) * 0.0625f;
    t = (c >> 4) * 0.0625f;

    R_DrawImage(x, y, CHAR_WIDTH, CHAR_HEIGHT, s, t, s + 0.0625f, t + 0.0625f,
                draw.colors[c >> 7].u32, image, false);
}

void R_DrawChar_SW(int x, int y, int flags, int c, qhandle_t font)
{
    draw_char(x, y, flags, c & 255, IMG_ForHandle(font));
}

int R_DrawString_SW(int x, int y, int flags, size_t maxlen, const char *s, qhandle_t font)
{
    image_t *image = IMG_ForHandle(font);

    while (maxlen-- && *s) {
        byte c = *s++;
        draw_char(x, y, flags, c, image);
        x += CHAR_WIDTH;
    }

    return x;
}
