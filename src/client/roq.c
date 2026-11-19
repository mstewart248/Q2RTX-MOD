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
*/

/*
==============================================================================

ROQ - Quake III Arena's videos

The campaign's intro, its tier videos and its ending (video/*.RoQ in Quake
III's paks), played by the campaign's video page (client/ui/q3sp.c). The
format, as Quake III's cl_cin.c and ffmpeg's roqvideodec.c read it:

  - chunks of an 8-byte header - id (16 bits), size (32), argument (16) -
    and their data, all little-endian;
  - 0x1084 starts the file, its argument the frame rate;
  - 0x1001 gives the picture's size;
  - 0x1002 is a codebook: 2x2 cells (four Y, one U, one V each, full-range
    YUV) and 4x4 cells made of four 2x2 cells;
  - 0x1011 is a frame: 16x16 blocks of four 8x8 blocks, each 8x8 block kept
    from the buffer (MOT), moved from the last frame (FCC), drawn from a 4x4
    cell doubled (SLD), or split into four 4x4 blocks that take the same
    four codes one level down (CCC). The two buffers take turns, so a kept
    block is the one from two frames ago - as Quake III decodes it, which is
    what the encoder planned for;
  - 0x1020 / 0x1021 are sound, mono or stereo, 22050 Hz, each byte a step
    of the square-law DPCM from the last sample.

The whole file is read at once; a tier video is a few megabytes.

==============================================================================
*/

#include "client.h"

#define ROQ_HEADER          0x1084
#define ROQ_INFO            0x1001
#define ROQ_QUAD_CODEBOOK   0x1002
#define ROQ_QUAD_VQ         0x1011
#define ROQ_SOUND_MONO      0x1020
#define ROQ_SOUND_STEREO    0x1021

#define ROQ_ID_MOT          0
#define ROQ_ID_FCC          1
#define ROQ_ID_SLD          2
#define ROQ_ID_CCC          3

#define ROQ_SOUND_RATE      22050
#define ROQ_MAX_SIZE        2048

typedef struct {
    byte        *file;
    int         len, pos;
    bool        open, done;

    int         width, height;
    int         fps;

    uint32_t    cb2[256][4];        // 2x2 cells, RGBA, row by row
    byte        cb4[256][4];        // 4x4 cells: four 2x2 cell numbers

    uint32_t    *buf[2];
    int         cur;                // the buffer the next frame goes into
    int         decoded;            // frames decoded so far

    unsigned    start_time;
    qhandle_t   image;
    int         image_frame;

    int16_t     *samples;
    int         samples_cap;
} roq_t;

static roq_t roq;

static int RoQ_Clip(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static uint32_t RoQ_YUV(int y, int u, int v)
{
    // full-range YCbCr, as Quake III's tables have it
    float r = y + 1.40200f * (v - 128);
    float g = y - 0.34414f * (u - 128) - 0.71414f * (v - 128);
    float b = y + 1.77200f * (u - 128);

    return MakeColor(RoQ_Clip((int)(r + 0.5f)), RoQ_Clip((int)(g + 0.5f)), RoQ_Clip((int)(b + 0.5f)), 255);
}

static unsigned RoQ_U16(const byte *p)
{
    return p[0] | (p[1] << 8);
}

static unsigned RoQ_U32(const byte *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

void RoQ_Close(void)
{
    if (roq.image)
        R_UnregisterImage(roq.image);
    if (roq.file)
        FS_FreeFile(roq.file);
    Z_Free(roq.buf[0]);
    Z_Free(roq.buf[1]);
    Z_Free(roq.samples);
    memset(&roq, 0, sizeof(roq));
}

bool RoQ_IsOpen(void)
{
    return roq.open;
}

/*
=================
RoQ_Open

Path as the filesystem takes it ("video/tier1.RoQ"). The first frame is due
straight away.
=================
*/
bool RoQ_Open(const char *path)
{
    void    *data;
    int     len;

    RoQ_Close();

    len = FS_LoadFile(path, &data);
    if (len < 8 || !data) {
        if (data)
            FS_FreeFile(data);
        Com_DPrintf("RoQ: couldn't read %s\n", path);
        return false;
    }

    roq.file = data;
    roq.len = len;

    if (RoQ_U16(roq.file) != ROQ_HEADER) {
        Com_WPrintf("RoQ: %s is not a RoQ video\n", path);
        RoQ_Close();
        return false;
    }

    roq.fps = RoQ_U16(roq.file + 6);
    if (roq.fps <= 0 || roq.fps > 120)
        roq.fps = 30;
    roq.pos = 8;
    roq.open = true;
    roq.start_time = cls.realtime;
    return true;
}

// ---- pictures ----

static void RoQ_Cell2(uint32_t *dst, int x, int y, const uint32_t *cell)
{
    uint32_t *p;

    if (x < 0 || y < 0 || x + 2 > roq.width || y + 2 > roq.height)
        return;
    p = dst + y * roq.width + x;
    p[0] = cell[0];
    p[1] = cell[1];
    p[roq.width] = cell[2];
    p[roq.width + 1] = cell[3];
}

// a 2x2 cell drawn twice the size, over 4x4
static void RoQ_Cell2x2(uint32_t *dst, int x, int y, const uint32_t *cell)
{
    int i, j;

    if (x < 0 || y < 0 || x + 4 > roq.width || y + 4 > roq.height)
        return;
    for (j = 0; j < 4; j++) {
        uint32_t *p = dst + (y + j) * roq.width + x;
        for (i = 0; i < 4; i++)
            p[i] = cell[(j >> 1) * 2 + (i >> 1)];
    }
}

// a 4x4 cell: four 2x2 cells
static void RoQ_Cell4(uint32_t *dst, int x, int y, int n)
{
    const byte *c = roq.cb4[n];

    RoQ_Cell2(dst, x, y, roq.cb2[c[0]]);
    RoQ_Cell2(dst, x + 2, y, roq.cb2[c[1]]);
    RoQ_Cell2(dst, x, y + 2, roq.cb2[c[2]]);
    RoQ_Cell2(dst, x + 2, y + 2, roq.cb2[c[3]]);
}

// a 4x4 cell twice the size, over 8x8
static void RoQ_Cell4x2(uint32_t *dst, int x, int y, int n)
{
    const byte *c = roq.cb4[n];

    RoQ_Cell2x2(dst, x, y, roq.cb2[c[0]]);
    RoQ_Cell2x2(dst, x + 4, y, roq.cb2[c[1]]);
    RoQ_Cell2x2(dst, x, y + 4, roq.cb2[c[2]]);
    RoQ_Cell2x2(dst, x + 4, y + 4, roq.cb2[c[3]]);
}

// size x size from the last frame, (mx, my) away
static void RoQ_Motion(uint32_t *dst, const uint32_t *src, int x, int y, int mx, int my, int size)
{
    int sx = x + mx, sy = y + my, j;

    if (x < 0 || y < 0 || x + size > roq.width || y + size > roq.height)
        return;
    if (sx < 0 || sy < 0 || sx + size > roq.width || sy + size > roq.height)
        return;
    for (j = 0; j < size; j++)
        memcpy(dst + (y + j) * roq.width + x, src + (sy + j) * roq.width + sx, size * sizeof(uint32_t));
}

static void RoQ_Codebook(const byte *data, unsigned size, unsigned arg)
{
    unsigned n2 = (arg >> 8) & 0xff, n4 = arg & 0xff, i;

    if (!n2)
        n2 = 256;
    if (!n4 && n2 * 6 < size)
        n4 = 256;
    if (n2 * 6 + n4 * 4 > size)
        return;

    for (i = 0; i < n2; i++, data += 6) {
        roq.cb2[i][0] = RoQ_YUV(data[0], data[4], data[5]);
        roq.cb2[i][1] = RoQ_YUV(data[1], data[4], data[5]);
        roq.cb2[i][2] = RoQ_YUV(data[2], data[4], data[5]);
        roq.cb2[i][3] = RoQ_YUV(data[3], data[4], data[5]);
    }
    for (i = 0; i < n4; i++, data += 4)
        memcpy(roq.cb4[i], data, 4);
}

static void RoQ_Frame(const byte *data, unsigned size, unsigned arg)
{
    const byte  *end = data + size;
    uint32_t    *dst = roq.buf[roq.cur];
    uint32_t    *src = roq.buf[roq.cur ^ 1];
    int         mean_x = (signed char)(arg >> 8), mean_y = (signed char)(arg & 0xff);
    int         xpos = 0, ypos = 0, xp, yp, k, flag_pos = -1;
    unsigned    flags = 0;

#define ROQ_NEXT_CODE(out) do { \
        if (flag_pos < 0) { \
            if (data + 2 > end) goto done; \
            flags = RoQ_U16(data); data += 2; flag_pos = 7; \
        } \
        (out) = (flags >> (flag_pos * 2)) & 3; flag_pos--; \
    } while (0)
#define ROQ_NEXT_BYTE(out) do { if (data >= end) goto done; (out) = *data++; } while (0)

    // the second frame starts from the first: nothing from two frames back yet
    if (roq.decoded == 1)
        memcpy(dst, src, roq.width * roq.height * sizeof(uint32_t));

    while (data < end && ypos < roq.height) {
        for (yp = ypos; yp < ypos + 16; yp += 8) {
            for (xp = xpos; xp < xpos + 16; xp += 8) {
                int code, b;

                ROQ_NEXT_CODE(code);
                switch (code) {
                case ROQ_ID_MOT:
                    break;
                case ROQ_ID_FCC:
                    ROQ_NEXT_BYTE(b);
                    RoQ_Motion(dst, src, xp, yp, 8 - (b >> 4) - mean_x, 8 - (b & 15) - mean_y, 8);
                    break;
                case ROQ_ID_SLD:
                    ROQ_NEXT_BYTE(b);
                    RoQ_Cell4x2(dst, xp, yp, b);
                    break;
                case ROQ_ID_CCC:
                    for (k = 0; k < 4; k++) {
                        int x = xp + ((k & 1) ? 4 : 0);
                        int y = yp + ((k & 2) ? 4 : 0);
                        int sub, c0, c1, c2, c3;

                        ROQ_NEXT_CODE(sub);
                        switch (sub) {
                        case ROQ_ID_MOT:
                            break;
                        case ROQ_ID_FCC:
                            ROQ_NEXT_BYTE(b);
                            RoQ_Motion(dst, src, x, y, 8 - (b >> 4) - mean_x, 8 - (b & 15) - mean_y, 4);
                            break;
                        case ROQ_ID_SLD:
                            ROQ_NEXT_BYTE(b);
                            RoQ_Cell4(dst, x, y, b);
                            break;
                        case ROQ_ID_CCC:
                            ROQ_NEXT_BYTE(c0);
                            ROQ_NEXT_BYTE(c1);
                            ROQ_NEXT_BYTE(c2);
                            ROQ_NEXT_BYTE(c3);
                            RoQ_Cell2(dst, x, y, roq.cb2[c0]);
                            RoQ_Cell2(dst, x + 2, y, roq.cb2[c1]);
                            RoQ_Cell2(dst, x, y + 2, roq.cb2[c2]);
                            RoQ_Cell2(dst, x + 2, y + 2, roq.cb2[c3]);
                            break;
                        }
                    }
                    break;
                }
            }
        }
        xpos += 16;
        if (xpos >= roq.width) {
            xpos -= roq.width;
            ypos += 16;
        }
    }

done:
#undef ROQ_NEXT_CODE
#undef ROQ_NEXT_BYTE
    roq.cur ^= 1;
    roq.decoded++;
}

// ---- sound ----

static void RoQ_Sound(const byte *data, unsigned size, unsigned arg, bool stereo)
{
    int         l, r, i, n;
    int16_t     *out;

    if (!size)
        return;
    if ((int)size > roq.samples_cap) {
        Z_Free(roq.samples);
        roq.samples = Z_Malloc(size * sizeof(int16_t));
        roq.samples_cap = size;
    }
    out = roq.samples;

    if (stereo) {
        l = (int16_t)(arg & 0xff00);
        r = (int16_t)((arg & 0xff) << 8);
        n = size / 2;
        for (i = 0; i < n; i++) {
            int dl = data[i * 2], dr = data[i * 2 + 1];

            l += dl < 128 ? dl * dl : -(dl - 128) * (dl - 128);
            r += dr < 128 ? dr * dr : -(dr - 128) * (dr - 128);
            l = (l < -32768 ? -32768 : l > 32767 ? 32767 : l);
            r = (r < -32768 ? -32768 : r > 32767 ? 32767 : r);
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }
        S_RawSamples(n, ROQ_SOUND_RATE, 2, 2, (byte *)out, 1.0f);
    } else {
        l = (int16_t)arg;
        for (i = 0; i < (int)size; i++) {
            int d = data[i];

            l += d < 128 ? d * d : -(d - 128) * (d - 128);
            l = (l < -32768 ? -32768 : l > 32767 ? 32767 : l);
            out[i] = l;
        }
        S_RawSamples(size, ROQ_SOUND_RATE, 2, 1, (byte *)out, 1.0f);
    }
}

// the chunks up to and including the next frame; false at the end
static bool RoQ_DecodeNext(void)
{
    while (roq.pos + 8 <= roq.len) {
        const byte  *h = roq.file + roq.pos;
        unsigned    id = RoQ_U16(h), size = RoQ_U32(h + 2), arg = RoQ_U16(h + 6);
        const byte  *data = h + 8;

        if (size > (unsigned)(roq.len - roq.pos - 8))
            return false;   // cut short
        roq.pos += 8 + size;

        switch (id) {
        case ROQ_INFO:
            if (size >= 4 && !roq.buf[0]) {
                int w = RoQ_U16(data), hgt = RoQ_U16(data + 2);

                if (w <= 0 || hgt <= 0 || w > ROQ_MAX_SIZE || hgt > ROQ_MAX_SIZE)
                    return false;
                roq.width = w;
                roq.height = hgt;
                roq.buf[0] = Z_Mallocz(w * hgt * sizeof(uint32_t));
                roq.buf[1] = Z_Mallocz(w * hgt * sizeof(uint32_t));
            }
            break;
        case ROQ_QUAD_CODEBOOK:
            RoQ_Codebook(data, size, arg);
            break;
        case ROQ_QUAD_VQ:
            if (!roq.buf[0])
                break;
            RoQ_Frame(data, size, arg);
            return true;
        case ROQ_SOUND_MONO:
            RoQ_Sound(data, size, arg, false);
            break;
        case ROQ_SOUND_STEREO:
            RoQ_Sound(data, size, arg, true);
            break;
        default:
            break;      // the JPEG frame and the rest: not in Quake III's videos
        }
    }
    return false;
}

/*
=================
RoQ_Update

Decode every frame that is due by now. False once the video has ended.
=================
*/
bool RoQ_Update(void)
{
    int due, n = 0;

    if (!roq.open || roq.done)
        return false;

    due = (int)((int64_t)(cls.realtime - roq.start_time) * roq.fps / 1000) + 1;

    // a long stall (a load, the window minimised): pick up from here, not
    // decode the whole backlog
    if (due - roq.decoded > roq.fps) {
        roq.start_time = cls.realtime - roq.decoded * 1000 / roq.fps;
        due = roq.decoded + 1;
    }

    while (roq.decoded < due) {
        if (!RoQ_DecodeNext()) {
            roq.done = true;
            break;
        }
        n++;
    }

    if (n && roq.decoded > roq.image_frame) {
        // the frame just decoded is in the buffer before cur
        const uint32_t  *frame = roq.buf[roq.cur ^ 1];
        size_t          bytes = roq.width * roq.height * sizeof(uint32_t);
        byte            *pic = Z_Malloc(bytes);

        memcpy(pic, frame, bytes);
        if (roq.image)
            R_UnregisterImage(roq.image);
        roq.image = R_RegisterRawImage(va("**roq[%d]**", roq.decoded), roq.width, roq.height, pic,
                                       IT_SPRITE, IF_SRGB | IF_NO_MIPMAPS | IF_VIDEO);
        roq.image_frame = roq.decoded;
    }

    return !roq.done;
}

qhandle_t RoQ_Image(int *width, int *height)
{
    if (width)
        *width = roq.width;
    if (height)
        *height = roq.height;
    return roq.image;
}
