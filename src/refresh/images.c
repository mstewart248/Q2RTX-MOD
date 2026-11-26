/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2003-2008 Andrey Nazarov
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

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

//
// images.c -- image reading and writing functions
//

#include "shared/shared.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/files.h"
#include "common/bsp.h"
#include "../client/client.h"
#include "refresh/images.h"
#include "refresh/models.h"
#include "system/system.h"
#include "format/pcx.h"
#include "format/wal.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <assert.h>

#define R_COLORMAP_PCX    "pics/colormap.pcx"

#define IMG_LOAD(x) \
    static int IMG_Load##x(byte *rawdata, size_t rawlen, \
        image_t *image, byte **pic)

typedef struct screenshot_s {
    int (*save_cb)(struct screenshot_s *);
    byte *pixels;
    FILE *fp;
    char *filename;
    int width, height, row_stride, status, param;
    bool async;
} screenshot_t;

void stbi_write(void *context, void *data, int size)
{
	fwrite(data, size, 1, ((screenshot_t *) context)->fp);
}

extern cvar_t* vid_rtx;
extern cvar_t* gl_use_hd_assets;

// GL without "use new textures and models", and the software renderer
// always, draw the classic .wal/.pcx world and model textures
static bool classic_assets(void)
{
    return (cls.ref_type == REF_TYPE_GL && !gl_use_hd_assets->integer) ||
           cls.ref_type == REF_TYPE_SW;
}

/*
====================================================================

IMAGE FLOOD FILLING

====================================================================
*/

typedef struct {
    short       x, y;
} floodfill_t;

// must be a power of 2
#define FLOODFILL_FIFO_SIZE 0x1000
#define FLOODFILL_FIFO_MASK (FLOODFILL_FIFO_SIZE - 1)

#define FLOODFILL_STEP(off, dx, dy) \
    do { \
        if (pos[off] == fillcolor) { \
            pos[off] = 255; \
            fifo[inpt].x = x + (dx); \
            fifo[inpt].y = y + (dy); \
            inpt = (inpt + 1) & FLOODFILL_FIFO_MASK; \
        } else if (pos[off] != 255) { \
            fdc = pos[off]; \
        } \
    } while(0)

/*
=================
IMG_FloodFill

Fill background pixels so mipmapping doesn't have haloes
=================
*/
static q_noinline void IMG_FloodFill(byte *skin, int skinwidth, int skinheight)
{
    byte                fillcolor = *skin; // assume this is the pixel to fill
    floodfill_t         fifo[FLOODFILL_FIFO_SIZE];
    int                 inpt = 0, outpt = 0;
    int                 filledcolor = 0; // FIXME: fixed black

    // can't fill to filled color or to transparent color
    // (used as visited marker)
    if (fillcolor == filledcolor || fillcolor == 255) {
        return;
    }

    fifo[inpt].x = 0, fifo[inpt].y = 0;
    inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

    while (outpt != inpt) {
        int         x = fifo[outpt].x, y = fifo[outpt].y;
        int         fdc = filledcolor;
        byte        *pos = &skin[x + skinwidth * y];

        outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

        if (x > 0) FLOODFILL_STEP(-1, -1, 0);
        if (x < skinwidth - 1) FLOODFILL_STEP(1, 1, 0);
        if (y > 0) FLOODFILL_STEP(-skinwidth, 0, -1);
        if (y < skinheight - 1) FLOODFILL_STEP(skinwidth, 0, 1);

        skin[x + skinwidth * y] = fdc;
    }
}

/*
=================================================================

PCX LOADING

=================================================================
*/

static int IMG_DecodePCX(byte *rawdata, size_t rawlen, byte *pixels,
                         byte *palette, int *width, int *height)
{
    byte    *raw, *end;
    dpcx_t  *pcx;
    int     x, y, w, h, scan;
    int     dataByte, runLength;

    //
    // parse the PCX file
    //
    if (rawlen < sizeof(dpcx_t)) {
        return Q_ERR_FILE_TOO_SMALL;
    }

    pcx = (dpcx_t *)rawdata;

    if (pcx->manufacturer != 10 || pcx->version != 5) {
        return Q_ERR_UNKNOWN_FORMAT;
    }

    if (pcx->encoding != 1 || pcx->bits_per_pixel != 8) {
        return Q_ERR_INVALID_FORMAT;
    }

    w = (LittleShort(pcx->xmax) - LittleShort(pcx->xmin)) + 1;
    h = (LittleShort(pcx->ymax) - LittleShort(pcx->ymin)) + 1;
    if (w < 1 || h < 1 || w > 640 || h > 480) {
        return Q_ERR_INVALID_FORMAT;
    }

    if (pcx->color_planes != 1) {
        return Q_ERR_INVALID_FORMAT;
    }

    scan = LittleShort(pcx->bytes_per_line);
    if (scan < w) {
        return Q_ERR_INVALID_FORMAT;
    }

    //
    // get palette
    //
    if (palette) {
        if (rawlen < 768) {
            return Q_ERR_FILE_TOO_SMALL;
        }
        memcpy(palette, (byte *)pcx + rawlen - 768, 768);
    }

    //
    // get pixels
    //
    if (pixels) {
        raw = pcx->data;
        end = (byte *)pcx + rawlen;
        for (y = 0; y < h; y++, pixels += w) {
            for (x = 0; x < scan;) {
                if (raw >= end)
                    return Q_ERR_BAD_RLE_PACKET;
                dataByte = *raw++;

                if ((dataByte & 0xC0) == 0xC0) {
                    runLength = dataByte & 0x3F;
                    if (x + runLength > scan)
                        return Q_ERR_BAD_RLE_PACKET;
                    if (raw >= end)
                        return Q_ERR_BAD_RLE_PACKET;
                    dataByte = *raw++;
                } else {
                    runLength = 1;
                }

                while (runLength--) {
                    if (x < w)
                        pixels[x] = dataByte;
                    x++;
                }
            }
        }
    }

    if (width)
        *width = w;
    if (height)
        *height = h;

    return Q_ERR_SUCCESS;
}

/*
===============
IMG_Unpack8
===============
*/
static int IMG_Unpack8(uint32_t *out, const uint8_t *in, int width, int height)
{
    int         x, y, p;
    bool        has_alpha = false;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            p = *in;
            if (p == 255) {
                has_alpha = true;
                // transparent, so scan around for another color
                // to avoid alpha fringes
                if (y > 0 && *(in - width) != 255)
                    p = *(in - width);
                else if (y < height - 1 && *(in + width) != 255)
                    p = *(in + width);
                else if (x > 0 && *(in - 1) != 255)
                    p = *(in - 1);
                else if (x < width - 1 && *(in + 1) != 255)
                    p = *(in + 1);
                else if (y > 0 && x > 0 && *(in - width - 1) != 255)
                    p = *(in - width - 1);
                else if (y > 0 && x < width - 1 && *(in - width + 1) != 255)
                    p = *(in - width + 1);
                else if (y < height - 1 && x > 0 && *(in + width - 1) != 255)
                    p = *(in + width - 1);
                else if (y < height - 1 && x < width - 1 && *(in + width + 1) != 255)
                    p = *(in + width + 1);
                else
                    p = 0;
                // copy rgb components
                *out = d_8to24table[p] & U32_RGB;
            } else {
                *out = d_8to24table[p];
            }
            in++;
            out++;
        }
    }

    if (has_alpha)
        return IF_PALETTED | IF_TRANSPARENT;

    return IF_PALETTED | IF_OPAQUE;
}

IMG_LOAD(PCX)
{
    byte        buffer[640 * 480];
    int         w, h;
    int         ret;

    ret = IMG_DecodePCX(rawdata, rawlen, buffer, NULL, &w, &h);
    if (ret < 0)
        return ret;

    if (image->type == IT_SKIN)
        IMG_FloodFill(buffer, w, h);

    *pic = IMG_AllocPixels(w * h * 4);

    image->upload_width = image->width = w;
    image->upload_height = image->height = h;
    image->flags |= IMG_Unpack8((uint32_t *)*pic, buffer, w, h);

    return Q_ERR_SUCCESS;
}


/*
=================================================================

WAL LOADING

=================================================================
*/

IMG_LOAD(WAL)
{
    miptex_t    *mt;
    size_t      w, h, offset, size, endpos;

    if (rawlen < sizeof(miptex_t)) {
        return Q_ERR_FILE_TOO_SMALL;
    }

    mt = (miptex_t *)rawdata;

    w = LittleLong(mt->width);
    h = LittleLong(mt->height);
    if (w < 1 || h < 1 || w > 512 || h > 512) {
        return Q_ERR_INVALID_FORMAT;
    }

    size = w * h;

    offset = LittleLong(mt->offsets[0]);
    endpos = offset + size;
    if (endpos < offset || endpos > rawlen) {
        return Q_ERR_BAD_EXTENT;
    }

    *pic = IMG_AllocPixels(size * 4);

    image->upload_width = image->width = w;
    image->upload_height = image->height = h;
    image->flags |= IMG_Unpack8((uint32_t *)*pic, (uint8_t *)mt + offset, w, h);

    return Q_ERR_SUCCESS;
}

/*
=================================================================

STB_IMAGE LOADING

=================================================================
*/

// PF_R16_UNORM: both path tracers upload it (d3d12/textures.c maps it to R16_UNORM)
static bool supports_extended_pixel_format(void)
{
	return CL_PathTraced();
}

IMG_LOAD(STB)
{
	int w, h, channels;
	byte* data = NULL;
	if(supports_extended_pixel_format())
	{
		int img_comp;
		stbi_info_from_memory(rawdata, rawlen, NULL, NULL, &img_comp);
		bool img_is_16 = stbi_is_16_bit_from_memory(rawdata, rawlen);

		if(img_comp == 1 && img_is_16)
		{
			// Special: 16bpc grayscale
			data = (byte*)stbi_load_16_from_memory(rawdata, rawlen, &w, &h, &channels, 1);
			image->pixel_format = PF_R16_UNORM;
		}
		// else: handle default case (8bpc RGBA) below
	}
	if(!data)
	{
		data = stbi_load_from_memory(rawdata, rawlen, &w, &h, &channels, 4);
		image->pixel_format = PF_R8G8B8A8_UNORM;
	}

	if (!data)
		return Q_ERR_LIBRARY_ERROR;

	*pic = data;

	image->upload_width = image->width = w;
	image->upload_height = image->height = h;

	if (channels == 3)
		image->flags |= IF_OPAQUE;

    return Q_ERR_SUCCESS;
}


/*
=================================================================

STB_IMAGE SAVING

=================================================================
*/

static int IMG_SaveTGA(screenshot_t *s)
{
	stbi_flip_vertically_on_write(1);
	int ret = stbi_write_tga_to_func(stbi_write, s, s->width, s->height, 3, s->pixels);

	if (ret) 
		return Q_ERR_SUCCESS;

	return Q_ERR_LIBRARY_ERROR;
}

static int IMG_SaveJPG(screenshot_t *s)
{
	stbi_flip_vertically_on_write(1);
	int ret = stbi_write_jpg_to_func(stbi_write, s, s->width, s->height, 3, s->pixels, s->param);

	if (ret)
		return Q_ERR_SUCCESS;

	return Q_ERR_LIBRARY_ERROR;
}


/*
=================
IMG_GrabLevelshot

What the levelshot generator saves (client/levelshot.c): the frame read back,
the middle 4:3 of it, each output pixel the average of the screen pixels it
covers - in linear light, or a shrunk bright edge goes dark - encoded as a JPEG
in memory. The caller writes it through the filesystem, so it lands under the
game's write directory and the menus find it as levelshots/<map>.jpg. Rows stay
bottom-up throughout, as IMG_ReadPixels hands them over and as the other
writers expect.

Handed back rather than written so the caller can look at *brightness first -
the picture's mean luma, 0..255 in sRGB (Rec. 601 weights) - and try another
camera when this one came out black, keeping the brightest grab if none do.
=================
*/
typedef struct {
    byte    *data;
    size_t  len, cap;
} levelshot_buf_t;

static bool is_render_hdr(void);

static void levelshot_write(void *context, void *data, int size)
{
    levelshot_buf_t *b = context;

    if (size <= 0)
        return;
    if (b->len + size > b->cap) {
        size_t cap = max(b->cap * 2, b->len + size + 65536);
        b->data = Z_Realloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, size);
    b->len += size;
}

int IMG_GrabLevelshot(int width, int height, int quality, void **data, size_t *len, float *brightness)
{
    static float    to_linear[256];
    static bool     table_ready;
    byte            *pixels, *out;
    int             w, h, stride, crop_w, crop_h, x0, y0, x, y, i, ret;
    double          luma = 0;
    levelshot_buf_t buf = { 0 };

    *data = NULL;
    *len = 0;
    *brightness = 0;

    if (!IMG_ReadPixels || width <= 0 || height <= 0)
        return Q_ERR(EINVAL);
    if (is_render_hdr() && !IMG_ReadPixelsTakesHDR)
        return Q_ERR(ENOSYS);

    if (!table_ready) {
        for (i = 0; i < 256; i++) {
            float c = i / 255.0f;
            to_linear[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
        }
        table_ready = true;
    }

    pixels = IMG_ReadPixels(&w, &h, &stride);
    if (!pixels)
        return Q_ERR(EIO);

    // the widest 4:3 that fits, centred
    crop_w = w;
    crop_h = w * 3 / 4;
    if (crop_h > h) {
        crop_h = h;
        crop_w = h * 4 / 3;
    }
    x0 = (w - crop_w) / 2;
    y0 = (h - crop_h) / 2;

    out = Z_Malloc(width * height * 3);
    for (y = 0; y < height; y++) {
        int sy0 = y0 + y * crop_h / height;
        int sy1 = max(y0 + (y + 1) * crop_h / height, sy0 + 1);

        for (x = 0; x < width; x++) {
            int     sx0 = x0 + x * crop_w / width;
            int     sx1 = max(x0 + (x + 1) * crop_w / width, sx0 + 1);
            float   sum[3] = { 0, 0, 0 };
            int     n = 0, sx, sy, c;

            for (sy = sy0; sy < sy1; sy++) {
                const byte *row = pixels + sy * stride + sx0 * 3;
                for (sx = sx0; sx < sx1; sx++, row += 3) {
                    sum[0] += to_linear[row[0]];
                    sum[1] += to_linear[row[1]];
                    sum[2] += to_linear[row[2]];
                    n++;
                }
            }
            for (c = 0; c < 3; c++) {
                float v = sum[c] / n;
                v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
                out[(y * width + x) * 3 + c] = (byte)max(0, min(255, (int)(v * 255.0f + 0.5f)));
            }
            luma += 0.299 * out[(y * width + x) * 3 + 0] + 0.587 * out[(y * width + x) * 3 + 1] +
                    0.114 * out[(y * width + x) * 3 + 2];
        }
    }
    Z_Free(pixels);

    stbi_flip_vertically_on_write(1);
    if (stbi_write_jpg_to_func(levelshot_write, &buf, width, height, 3, out, quality) && buf.len) {
        *data = buf.data;
        *len = buf.len;
        *brightness = (float)(luma / ((double)width * height));
        ret = Q_ERR_SUCCESS;
    } else {
        Z_Free(buf.data);
        ret = Q_ERR_LIBRARY_ERROR;
    }

    Z_Free(out);
    return ret;
}

static int IMG_SavePNG(screenshot_t *s)
{
	stbi_flip_vertically_on_write(1);
	int ret = stbi_write_png_to_func(stbi_write, s, s->width, s->height, 3, s->pixels, s->row_stride);

	if (ret)
		return Q_ERR_SUCCESS;

	return Q_ERR_LIBRARY_ERROR;
}

static int IMG_SaveHDR(screenshot_t *s)
{
	stbi_flip_vertically_on_write(1);
	// NOTE: The 'pixels' point is byte*, but HDR writing needs float*!
	int ret = stbi_write_hdr_to_func(stbi_write, s, s->width, s->height, 3, (float*)s->pixels);

	if (ret)
		return Q_ERR_SUCCESS;

	return Q_ERR_LIBRARY_ERROR;
}

/*
=========================================================

SCREEN SHOTS

=========================================================
*/

static cvar_t *r_screenshot_format;
static cvar_t *r_screenshot_quality;
static cvar_t *r_screenshot_async;
static cvar_t* r_screenshot_compression;
static cvar_t* r_screenshot_message;
static cvar_t *r_screenshot_template;

static int suffix_pos(const char *s, int ch)
{
    int pos = strlen(s);
    while (pos > 0 && s[pos - 1] == ch)
        pos--;
    return pos;
}

static int parse_template(cvar_t *var, char *buffer, size_t size)
{
    if (FS_NormalizePathBuffer(buffer, var->string, size) < size) {
        FS_CleanupPath(buffer);
        int start = suffix_pos(buffer, 'X');
        int width = strlen(buffer) - start;
        buffer[start] = 0;
        if (width >= 3 && width <= 9)
            return width;
    }

    Com_WPrintf("Bad value '%s' for '%s'. Falling back to '%s'.\n",
                var->string, var->name, var->default_string);
    Cvar_Reset(var);
    Q_strlcpy(buffer, "quake", size);
    return 3;
}

static int create_screenshot(char *buffer, size_t size, FILE **f,
                             const char *name, const char *ext)
{
    char temp[MAX_OSPATH];
    int i, ret, width, count;

    if (name && *name) {
        // save to user supplied name
        if (FS_NormalizePathBuffer(temp, name, sizeof(temp)) >= sizeof(temp)) {
            return Q_ERR(ENAMETOOLONG);
        }
        FS_CleanupPath(temp);
        if (Q_snprintf(buffer, size, "%s/screenshots/%s%s", fs_gamedir, temp, ext) >= size) {
            return Q_ERR(ENAMETOOLONG);
        }
        if ((ret = FS_CreatePath(buffer)) < 0) {
            return ret;
        }
        if (!(*f = fopen(buffer, "wb"))) {
            return Q_ERRNO;
        }
        return 0;
    }

    width = parse_template(r_screenshot_template, temp, sizeof(temp));

    // create the directory
    if (Q_snprintf(buffer, size, "%s/screenshots/%s", fs_gamedir, temp) >= size) {
        return Q_ERR(ENAMETOOLONG);
    }
    if ((ret = FS_CreatePath(buffer)) < 0) {
        return ret;
    }

    count = 1;
    for (i = 0; i < width; i++)
        count *= 10;

    // find a file name to save it to
    for (i = 0; i < count; i++) {
        if (Q_snprintf(buffer, size, "%s/screenshots/%s%0*d%s", fs_gamedir, temp, width, i, ext) >= size) {
            return Q_ERR(ENAMETOOLONG);
        }
        if ((*f = Q_fopen(buffer, "wxb"))) {
            return 0;
        }
        ret = Q_ERRNO;
        if (ret != Q_ERR(EEXIST)) {
            return ret;
        }
    }
    
    return Q_ERR_OUT_OF_SLOTS;
}

static bool is_render_hdr(void)
{
    return R_IsHDR && R_IsHDR();
}

static void screenshot_work_cb(void *arg)
{
    screenshot_t *s = arg;
    s->status = s->save_cb(s);
}

static void screenshot_done_cb(void *arg)
{
    screenshot_t *s = arg;

    if (fclose(s->fp) && !s->status)
        s->status = Q_ERRNO;
    Z_Free(s->pixels);

    if (s->status < 0) {
        Com_EPrintf("Couldn't write %s: %s\n", s->filename, Q_ErrorString(s->status));
        remove(s->filename);
    } else if (r_screenshot_message->integer) {
        Com_Printf("Wrote %s\n", s->filename);
    }

    if (s->async) {
        Z_Free(s->filename);
        Z_Free(s);
    }
}

static void make_screenshot(const char *name, const char *ext,
                            int (*save_cb)(struct screenshot_s *),
                            bool async, int param)
{
    char        buffer[MAX_OSPATH];
    byte        *pixels;
    FILE        *fp;
    int         w, h, ret, row_stride;
    
    // D3D12's readback tone maps an HDR frame to SDR (the profiler's captures rely
    // on it); Vulkan's reads only an 8-bit swapchain
    if(is_render_hdr() && !IMG_ReadPixelsTakesHDR) {
        Com_WPrintf("Screenshot format not supported in HDR mode\n");
        return;
    }
    ret = create_screenshot(buffer, sizeof(buffer), &fp, name, ext);
    if (ret < 0) {
        Com_EPrintf("Couldn't create screenshot: %s\n", Q_ErrorString(ret));
        return;
    }

    /* TEMPORARY fog-fade diagnostic (pt_fog_log 1).  The screenshot command
       writes nothing to the console, so there has never been a way to say
       WHICH frame state a shot was taken in.  This line lands in console.log
       between FOGGRID lines, so the classifier can be validated against the
       actual picture - the one check this investigation has never done. */
    if (Cvar_Get("pt_fog_log", "0", 0)->integer)
        Com_Printf("FOGSHOT %s\n", buffer);

    if (r_screenshot_message->integer && async)
        Com_Printf("Taking async screenshot...\n");

    pixels = IMG_ReadPixels(&w, &h, &row_stride);

    screenshot_t s = {
        .save_cb = save_cb,
        .pixels = pixels,
        .fp = fp,
        .filename = async ? Z_CopyString(buffer) : buffer,
        .width = w,
        .height = h,
        .row_stride = row_stride,
        .status = -1,
        .param = param,
        .async = async,
    };

    if (async) {
        asyncwork_t work = {
            .work_cb = screenshot_work_cb,
            .done_cb = screenshot_done_cb,
            .cb_arg = Z_CopyStruct(&s),
        };
        Sys_QueueAsyncWork(&work);
    } else {
        screenshot_work_cb(&s);
        screenshot_done_cb(&s);
    }
}

static void make_screenshot_hdr(const char *name, bool async)
{
    char        buffer[MAX_OSPATH];
    float       *pixels;
    int         ret;
    FILE        *fp;
    int         w, h;

    if(!is_render_hdr()) {
        Com_WPrintf("Screenshot format supported in HDR mode only\n");
        return;
    }

    ret = create_screenshot(buffer, sizeof(buffer), &fp, name, ".hdr");
    if (ret < 0) {
        Com_EPrintf("Couldn't create HDR screenshot: %s\n", Q_ErrorString(ret));
        return;
    }

    pixels = IMG_ReadPixelsHDR(&w, &h);

    screenshot_t s = {
        .save_cb = IMG_SaveHDR,
        .pixels = (byte*)pixels,
        .fp = fp,
        .filename = async ? Z_CopyString(buffer) : buffer,
        .width = w,
        .height = h,
        .row_stride = 0,
        .status = -1,
        .param = 0,
        .async = async,
    };

    if (async) {
        asyncwork_t work = {
            .work_cb = screenshot_work_cb,
            .done_cb = screenshot_done_cb,
            .cb_arg = Z_CopyStruct(&s),
        };
        Sys_QueueAsyncWork(&work);
    } else {
        screenshot_work_cb(&s);
        screenshot_done_cb(&s);
    }
}

/*
==================
IMG_ScreenShot_f

Standard function to take a screenshot. Saves in default format unless user
overrides format with a second argument. Screenshot name can't be
specified. This function is always compiled in to give a meaningful warning
if no formats are available.
==================
*/
static void IMG_ScreenShot_f(void)
{
    const char *s;

    if (Cmd_Argc() > 2) {
        Com_Printf("Usage: %s [format]\n", Cmd_Argv(0));
        return;
    }

    if (Cmd_Argc() > 1) {
        s = Cmd_Argv(1);
    } else {
        if(is_render_hdr())
            s = "hdr";
        else
        s = r_screenshot_format->string;
    }

    if (*s == 'h') {
        make_screenshot_hdr(NULL, r_screenshot_async->integer > 0);
        return;
    }

    if (*s == 'j') {
        make_screenshot(NULL, ".jpg", IMG_SaveJPG,
                        r_screenshot_async->integer > 0,
                        r_screenshot_quality->integer);
        return;
    }

    if (*s == 'p') {
        make_screenshot(NULL, ".png", IMG_SavePNG,
                        r_screenshot_async->integer > 0,
                        r_screenshot_compression->integer);
        return;
    }

    make_screenshot(NULL, ".tga", IMG_SaveTGA, r_screenshot_async->integer > 0, 0);
}

/*
==================
IMG_ScreenShotXXX_f

Specialized function to take a screenshot in specified format. Screenshot name
can be also specified, as well as quality and compression options.
==================
*/

static void IMG_ScreenShotTGA_f(void)
{
    if (Cmd_Argc() > 2) {
        Com_Printf("Usage: %s [name]\n", Cmd_Argv(0));
        return;
    }

    make_screenshot(Cmd_Argv(1), ".tga", IMG_SaveTGA, r_screenshot_async->integer > 0, 0);
}

static void IMG_ScreenShotJPG_f(void)
{
    int quality;

    if (Cmd_Argc() > 3) {
        Com_Printf("Usage: %s [name] [quality]\n", Cmd_Argv(0));
        return;
    }

    if (Cmd_Argc() > 2) {
        quality = atoi(Cmd_Argv(2));
    } else {
        quality = r_screenshot_quality->integer;
    }

    make_screenshot(Cmd_Argv(1), ".jpg", IMG_SaveJPG,
                    r_screenshot_async->integer > 0, quality);
}

static void IMG_ScreenShotPNG_f(void)
{
    int compression;

    if (Cmd_Argc() > 3) {
        Com_Printf("Usage: %s [name] [compression]\n", Cmd_Argv(0));
        return;
    }

    if (Cmd_Argc() > 2) {
        compression = atoi(Cmd_Argv(2));
    } else {
        compression = r_screenshot_compression->integer;
    }

    make_screenshot(Cmd_Argv(1), ".png", IMG_SavePNG,
                    r_screenshot_async->integer > 0, compression);
}

static void IMG_ScreenShotHDR_f(void)
{
    if (Cmd_Argc() > 2) {
        Com_Printf("Usage: %s [name]\n", Cmd_Argv(0));
        return;
    }

    make_screenshot_hdr(Cmd_Argv(1), r_screenshot_async->integer > 0);
}

/*
=========================================================

IMAGE PROCESSING

=========================================================
*/

void IMG_ResampleTexture(const byte *in, int inwidth, int inheight,
                         byte *out, int outwidth, int outheight)
{
    int i, j;
    const byte  *inrow1, *inrow2;
    unsigned    frac, fracstep;
    unsigned    p1[MAX_TEXTURE_SIZE], p2[MAX_TEXTURE_SIZE];
    const byte  *pix1, *pix2, *pix3, *pix4;
    float       heightScale;

    if (outwidth > MAX_TEXTURE_SIZE) {
        Com_Error(ERR_FATAL, "%s: outwidth > %d", __func__, MAX_TEXTURE_SIZE);
    }

    fracstep = inwidth * 0x10000 / outwidth;

    frac = fracstep >> 2;
    for (i = 0; i < outwidth; i++) {
        p1[i] = 4 * (frac >> 16);
        frac += fracstep;
    }
    frac = 3 * (fracstep >> 2);
    for (i = 0; i < outwidth; i++) {
        p2[i] = 4 * (frac >> 16);
        frac += fracstep;
    }

    heightScale = (float)inheight / outheight;
    inwidth <<= 2;
    for (i = 0; i < outheight; i++) {
        inrow1 = in + inwidth * (int)((i + 0.25f) * heightScale);
        inrow2 = in + inwidth * (int)((i + 0.75f) * heightScale);
        for (j = 0; j < outwidth; j++) {
            pix1 = inrow1 + p1[j];
            pix2 = inrow1 + p2[j];
            pix3 = inrow2 + p1[j];
            pix4 = inrow2 + p2[j];
            out[0] = (pix1[0] + pix2[0] + pix3[0] + pix4[0]) >> 2;
            out[1] = (pix1[1] + pix2[1] + pix3[1] + pix4[1]) >> 2;
            out[2] = (pix1[2] + pix2[2] + pix3[2] + pix4[2]) >> 2;
            out[3] = (pix1[3] + pix2[3] + pix3[3] + pix4[3]) >> 2;
            out += 4;
        }
    }
}

void IMG_MipMap(byte *out, byte *in, int width, int height)
{
    int     i, j;

    width <<= 2;
    height >>= 1;
    for (i = 0; i < height; i++, in += width) {
        for (j = 0; j < width; j += 8, out += 4, in += 8) {
            out[0] = (in[0] + in[4] + in[width + 0] + in[width + 4]) >> 2;
            out[1] = (in[1] + in[5] + in[width + 1] + in[width + 5]) >> 2;
            out[2] = (in[2] + in[6] + in[width + 2] + in[width + 6]) >> 2;
            out[3] = (in[3] + in[7] + in[width + 3] + in[width + 7]) >> 2;
        }
    }
}

/*
=========================================================

IMAGE MANAGER

=========================================================
*/

#define RIMAGES_HASH    256

static list_t   r_imageHash[RIMAGES_HASH];

image_t     r_images[MAX_RIMAGES];
int         r_numImages;

uint32_t    d_8to24table[256];

static const struct {
    char    ext[4];
    int     (*load)(byte *, size_t, image_t *, byte **);
} img_loaders[IM_MAX] = {
    { "pcx", IMG_LoadPCX },
    { "wal", IMG_LoadWAL },
    { "tga", IMG_LoadSTB },
    { "jpg", IMG_LoadSTB },
    { "png", IMG_LoadSTB }
};

static imageformat_t    img_search[IM_MAX];
static int              img_total;

static cvar_t   *r_override_textures;
static cvar_t   *r_texture_formats;

/*
===============
IMG_List_f
===============
*/
static void IMG_List_f(void)
{
	int        i;
	image_t    *image;
	int        texels, count;

	if (Cmd_Argc() > 1) {

		// save to file
		char path[MAX_OSPATH];

		qhandle_t f = FS_EasyOpenFile(path, sizeof(path), FS_MODE_WRITE | FS_FLAG_TEXT, "", Cmd_Argv(1), ".csv");
		if (!f) {
			Com_EPrintf("Error opening '%s'\n", path);
			return;
		}

		for (i = 1, count = 0, image = r_images + 1; i < r_numImages; i++, image++) {

			if (!image->registration_sequence)
				continue;

			char fmt[MAX_QPATH];
			sprintf(fmt, "%%-%ds, %%-%ds, (%% 5d %% 5d), sRGB:%%d\n", MAX_QPATH, MAX_QPATH);

			FS_FPrintf(f, fmt, 
				image->name, 
				image->filepath, 
				image->width, 
				image->height,
				image->is_srgb);
		}
		FS_CloseFile(f);

		Com_Printf("Saved '%s'\n", path);

	} else {

		// dump to console
		static const char types[8] = "PFMSWY??";

		Com_Printf("------------------\n");
		texels = count = 0;

		for (i = 1, image = r_images + 1; i < r_numImages; i++, image++) {
			if (!image->registration_sequence)
				continue;

			Com_Printf("%c%c%c%c %4i %4i %s: %s\n",
				types[image->type > IT_MAX ? IT_MAX : image->type],
				(image->flags & IF_TRANSPARENT) ? 'T' : ' ',
				(image->flags & IF_SCRAP) ? 'S' : ' ',
				(image->flags & IF_PERMANENT) ? '*' : ' ',
				image->upload_width,
				image->upload_height,
				(image->flags & IF_PALETTED) ? "PAL" : "RGB",
				image->name);

			texels += image->upload_width * image->upload_height;
			count++;
		}
		Com_Printf("Total images: %d (out of %d slots)\n", count, r_numImages);
		Com_Printf("Total texels: %d (not counting mipmaps)\n", texels);
	}
}

static image_t *alloc_image(void)
{
    int i;
    image_t *image, *placeholder = NULL;

    // find a free image_t slot
    for (i = 1, image = r_images + 1; i < r_numImages; i++, image++) {
        if (!image->registration_sequence)
            return image;
        if (!placeholder && image->type == IT_PIC && !(image->flags & IF_PERMANENT) &&
            !image->upload_width && !image->upload_height)
            placeholder = image;
    }

    // allocate new slot if possible
    if (r_numImages < MAX_RIMAGES) {
        r_numImages++;
        return image;
    }

    // reuse placeholder (failed temp pic) slot if available
    if (placeholder) {
        List_Remove(&placeholder->entry);
        memset(placeholder, 0, sizeof(*placeholder));
        return placeholder;
    }

    return NULL;
}

// finds the given image of the given type.
// case and extension insensitive.
static image_t *lookup_image(const char *name,
                             imagetype_t type, unsigned hash, size_t baselen)
{
    image_t *image;

    // look for it
    LIST_FOR_EACH(image_t, image, &r_imageHash[hash], entry) {
        if (image->type != type) {
            continue;
        }
        if (image->baselen != baselen) {
            continue;
        }
        if (!FS_pathcmpn(image->name, name, baselen)) {
            return image;
        }
    }

    return NULL;
}

/* lookup_image, but only an image read from exactly this file. lookup_image takes
   foo.jpg for foo.png - a Quake III picture id shipped as .jpg and a .png made
   beside it share one image, whichever was asked for first, and switching a .mat
   from one to the other showed nothing until a restart (Matt, 2026-11-17,
   base_light/ceil1_39.blend). See IMG_FindFile. */
static image_t *lookup_image_file(const char *name,
                                  imagetype_t type, unsigned hash, size_t baselen)
{
    image_t *image;

    LIST_FOR_EACH(image_t, image, &r_imageHash[hash], entry) {
        if (image->type != type || image->baselen != baselen) {
            continue;
        }
        if (image->filepath[0] && !FS_pathcmp(image->filepath, name)) {
            return image;
        }
    }

    return NULL;
}

#define TRY_IMAGE_SRC_GAME      1
#define TRY_IMAGE_SRC_BASE      0   // any path, game dir included
#define TRY_IMAGE_SRC_BASE_ONLY 2   // baseq2's own paths only

// Pics whose baseq2 copy wins over a game dir's copy of the same name. The
// rerelease's pics/i_health.pcx is the classic cross with its palette entry
// changed from red to green; the red one is wanted on the HUD in every game.
static const char *const prefer_base_pics[] = {
    "pics/i_health",
};

static bool prefer_base_pic(const char *name, size_t baselen)
{
    for (int i = 0; i < q_countof(prefer_base_pics); i++)
        if (strlen(prefer_base_pics[i]) == baselen && !FS_pathcmpn(name, prefer_base_pics[i], baselen))
            return true;
    return false;
}

static int _try_image_format(imageformat_t fmt, image_t *image, int try_src, byte **pic)
{
    byte        *data;
    int         len;
    int         ret;

    // load the file
    int fs_flags = 0;
    if (try_src > 0)
        fs_flags = try_src == TRY_IMAGE_SRC_GAME ? FS_PATH_GAME : FS_PATH_BASE;
    len = FS_LoadFileFlags(image->name, (void **)&data, fs_flags);
    if (!data) {
        return len;
    }

    // decompress the image
    ret = img_loaders[fmt].load(data, len, image, pic);

    FS_FreeFile(data);

    image->filepath[0] = 0;
    if (ret >= 0) {
        strcpy(image->filepath, image->name);
        // record last modified time (skips reload when invoking IMG_ReloadAll)
        image->last_modified = 0;
        FS_LastModified(image->filepath, &image->last_modified);
    }
    return ret < 0 ? ret : fmt;
}

static int try_image_format(imageformat_t fmt, image_t *image, int try_src, byte **pic)
{
    // replace the extension
    memcpy(image->name + image->baselen + 1, img_loaders[fmt].ext, 4);
    return _try_image_format(fmt, image, try_src, pic);
}


// tries to load the image with a different extension
static int try_other_formats(imageformat_t orig, image_t *image, int try_src, byte **pic)
{
    imageformat_t   fmt;
    int             ret;
    int             i;

    // search through all the 32-bit formats
    for (i = 0; i < img_total; i++) {
        fmt = img_search[i];
        if (fmt == orig) {
            continue;   // don't retry twice
        }

        ret = try_image_format(fmt, image, try_src, pic);
        if (ret != Q_ERR(ENOENT)) {
            return ret; // found something
        }
    }

    // fall back to 8-bit formats
    fmt = (image->type == IT_WALL) ? IM_WAL : IM_PCX;
    if (fmt == orig) {
        return Q_ERR(ENOENT); // don't retry twice
    }

    return try_image_format(fmt, image, try_src, pic);
}

// The size in the header of the .wal or .pcx at (name).
/*
=================================================================

QUAKE 1 WALL TEXTURES

A Quake 1 map carries its textures inside it, 8-bit in Quake's own palette,
and bsp_q1.c names them textures/q1/<name> ('*' spelled '#'). A file of that
name - an override, Matt's materialDev work - wins as for any wall; failing
one, the pixels come from the map (BSP_Q1FindMiptex).

<name>_light is made from the texture's fullbright colours, Quake's last 32,
which Quake draws unlit: the light fixtures, the lava glints, the computer
panels. Nothing else in a Quake 1 map tells the path tracer where its lights
are - Quake's own light entities only ever went into its lightmaps.

A '{' texture is a fence: palette index 255 is see-through there.

<sky>_q1sky0 and <sky>_q1sky1 are the two cloud layers of a sky texture
(256x128 in id's maps): the back one is its right half, solid; the front one
its left half, where colour 0 is see-through - as GLQuake's R_InitSky split
it. vkpt main.c's q1_sky_faces draws them.

=================================================================
*/

#define Q1_TEXTURE_PREFIX       "textures/q1/"
#define Q1_TEXTURE_PREFIX_LEN   12
#define Q1_FIRST_FULLBRIGHT     224

static uint32_t q1_palette[256];
static bool     q1_palette_loaded;

static bool q1_load_palette(void)
{
    byte *data;
    int len;

    if (q1_palette_loaded)
        return true;

    len = FS_LoadFile("id1/gfx/palette.lmp", (void **)&data);
    if (!data)
        return false;
    if (len < 768) {
        FS_FreeFile(data);
        return false;
    }
    for (int i = 0; i < 256; i++)
        q1_palette[i] = MakeColor(data[i * 3], data[i * 3 + 1], data[i * 3 + 2], 255);
    FS_FreeFile(data);
    q1_palette_loaded = true;
    return true;
}

// The pixels for textures/q1/<texture>[_light] (baselen: the name without
// its extension), from a loaded Quake 1 map. *light: the _light one was asked
// for. *fence: a '{' texture. *sky_layer: 0 or 1 for <sky>_q1sky0/1, the
// whole sky texture's pixels returned; -1 otherwise.
static const byte *q1_find_texture(const char *name, size_t baselen, int *w, int *h, bool *light, bool *fence,
                                   int *sky_layer)
{
    char base[MAX_QPATH];
    const byte *pix;
    size_t n;

    *sky_layer = -1;
    if (baselen <= Q1_TEXTURE_PREFIX_LEN || Q_strncasecmp(name, Q1_TEXTURE_PREFIX, Q1_TEXTURE_PREFIX_LEN))
        return NULL;
    n = baselen - Q1_TEXTURE_PREFIX_LEN;
    if (n >= sizeof(base))
        return NULL;
    memcpy(base, name + Q1_TEXTURE_PREFIX_LEN, n);
    base[n] = 0;
    *fence = base[0] == '{';

    *light = false;
    if ((pix = BSP_Q1FindMiptex(base, w, h)) != NULL)
        return pix;
    // a health or ammo box's (model_q1.c): those are not in the map
    if ((pix = MOD_Q1ItemMiptex(base, w, h)) != NULL)
        return pix;

    // a sky's cloud layer: half of the texture, as wide as it is high
    if (n > 7 && !Q_strncasecmp(base + n - 7, "_q1sky", 6) && (base[n - 1] == '0' || base[n - 1] == '1')) {
        const int layer = base[n - 1] - '0';
        base[n - 7] = 0;
        if ((pix = BSP_Q1FindMiptex(base, w, h)) == NULL || *w < 2)
            return NULL;
        *sky_layer = layer;
        return pix;
    }

    if (n > 6 && !Q_stricmp(base + n - 6, "_light")) {
        base[n - 6] = 0;
        *light = true;
        if ((pix = BSP_Q1FindMiptex(base, w, h)) != NULL)
            return pix;
        return MOD_Q1ItemMiptex(base, w, h);
    }
    return NULL;
}

static inline bool q1_is_fullbright(int p, bool fence)
{
    return p >= Q1_FIRST_FULLBRIGHT && !(fence && p == 255);
}

/*
Palette indices (src) to colour: all of them, or with (light) only the
fullbright ones, on black - NULL then when there are none, as nothing in it
glows. (fence): index 255 is see-through, and takes a solid neighbour's
colour so filtering leaves no dark fringe; (*alpha) says whether there was
any.
*/
static uint32_t *q1_expand_pixels(const byte *src, int w, int h, bool light, bool fence, bool *alpha)
{
    const size_t size = (size_t)w * h;
    uint32_t *out;

    *alpha = false;
    if (light) {
        size_t i;
        for (i = 0; i < size; i++)
            if (q1_is_fullbright(src[i], fence))
                break;
        if (i == size)
            return NULL;
    }

    out = (uint32_t *)IMG_AllocPixels(size * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int i = y * w + x;
            const int p = src[i];

            if (light) {
                out[i] = q1_is_fullbright(p, fence) ? q1_palette[p] : MakeColor(0, 0, 0, 255);
            } else if (fence && p == 255) {
                int q = 0;
                if (x > 0 && src[i - 1] != 255) q = src[i - 1];
                else if (x < w - 1 && src[i + 1] != 255) q = src[i + 1];
                else if (y > 0 && src[i - w] != 255) q = src[i - w];
                else if (y < h - 1 && src[i + w] != 255) q = src[i + w];
                out[i] = q1_palette[q] & U32_RGB;
                *alpha = true;
            } else {
                out[i] = q1_palette[p];
            }
        }
    }
    return out;
}

// what every image made from Quake 1's 8-bit pixels has in common
static void q1_finish_image(image_t *image, imagetype_t type, imageflags_t flags, int w, int h, bool alpha)
{
    image->type = type;
    image->flags = flags | IF_PALETTED | (alpha ? IF_TRANSPARENT : IF_OPAQUE);
    image->registration_sequence = registration_sequence;
    image->upload_width = image->width = w;
    image->upload_height = image->height = h;
    // no such file: it names what the pixels stand for in 'mat print', and
    // the texture reload finds nothing on disk to reload
    Q_strlcpy(image->filepath, image->name, sizeof(image->filepath));
    image->last_modified = 0;
}

// find_or_load_image's last resort for a Quake 1 wall, or a box's wall
// texture on its model (model_q1.c): (image)'s name is set
static int load_q1_texture(image_t *image, imagetype_t type, imageflags_t flags, byte **pic)
{
    const byte *src;
    uint32_t *out;
    bool light, fence, alpha = false;
    int w, h, sky_layer;

    if (type != IT_WALL && type != IT_SKIN)
        return Q_ERR(ENOENT);
    src = q1_find_texture(image->name, image->baselen, &w, &h, &light, &fence, &sky_layer);
    if (!src || !q1_load_palette())
        return Q_ERR(ENOENT);

    if (sky_layer >= 0) {
        // GLQuake's R_InitSky: the back layer is the right half; the front one
        // the left, its colour 0 see-through - and there the back layer's
        // average colour, so filtering leaves no dark fringe round the clouds
        const int half = w / 2;
        uint32_t sum[3] = { 0 };

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < half; x++) {
                const byte *c = (const byte *)&q1_palette[src[y * w + half + x]];
                sum[0] += c[0];
                sum[1] += c[1];
                sum[2] += c[2];
            }
        }
        const uint32_t n = (uint32_t)half * h;
        const uint32_t clear = MakeColor(sum[0] / n, sum[1] / n, sum[2] / n, 0);

        out = (uint32_t *)IMG_AllocPixels((size_t)half * h * 4);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < half; x++) {
                if (sky_layer == 0) {
                    out[y * half + x] = q1_palette[src[y * w + half + x]];
                } else {
                    const int p = src[y * w + x];
                    out[y * half + x] = p ? q1_palette[p] : clear;
                    alpha |= !p;
                }
            }
        }
        w = half;
    } else {
        out = q1_expand_pixels(src, w, h, light, fence, &alpha);
        if (!out)
            return Q_ERR(ENOENT);   // nothing in it glows
    }

    q1_finish_image(image, type, flags, w, h, alpha);
    *pic = (byte *)out;
    return Q_ERR_SUCCESS;
}

/*
The start of the last (tag) in (s) when all that follows it is (count)
numbers joined by '_' - "_mdl_" then "00_01", "_spr_" then "03" - with them
in (numbers); NULL otherwise. Exactly that, so a sidecar's name
(<model>_mdl_00_00_n) is not mistaken for the skin itself.
*/
static const char *q1_name_tag(const char *s, const char *tag, int count, int *numbers)
{
    const size_t taglen = strlen(tag);
    const char *found = NULL, *p;

    for (p = strstr(s, tag); p; p = strstr(p + 1, tag))
        found = p;
    if (!found)
        return NULL;

    p = found + taglen;
    for (int i = 0; i < count; i++) {
        if (i && *p++ != '_')
            return NULL;
        if (!Q_isdigit(*p))
            return NULL;
        numbers[i] = atoi(p);
        while (Q_isdigit(*p))
            p++;
    }
    return *p ? NULL : found;
}

/*
=================
load_q1_model_image

find_or_load_image's last resort for a Quake 1 model's skin or a sprite's
frame, with the names model_q1.c gives them - each keeping the "id1/<episode>/"
of its model, so an add-on's own pixels are its own:

  <path>.lmp              a rerelease md5 skin, read as a file: a width and a
                          height, then that many palette indices
  <model>_mdl_<S>_<F>     skin S (frame F of a skin group) cut out of <model>.mdl
  <sprite>_spr_<N>        frame N of <sprite>.spr, index 255 see-through

and any of them with _light after it: its fullbright colours alone, on black,
which Quake drew unlit - eyes, flames, the glow of a Quad Damage - and none
at all when it has none.
=================
*/
static int load_q1_model_image(image_t *image, imagetype_t type, imageflags_t flags, byte **pic)
{
    char base[MAX_QPATH], path[MAX_QPATH];
    byte *src = NULL;
    bool light = false, fence = false, alpha;
    int w = 0, h = 0, numbers[2], len;
    size_t n = image->baselen;
    const char *tag;
    uint32_t *out;

    if ((type != IT_SKIN && type != IT_SPRITE) || n >= sizeof(base))
        return Q_ERR(ENOENT);
    if (Q_strncasecmp(image->name, "id1/", 4) && Q_strncasecmp(image->name, "progs/", 6))
        return Q_ERR(ENOENT);
    if (!q1_load_palette())
        return Q_ERR(ENOENT);

    memcpy(base, image->name, n);
    base[n] = 0;
    if (n > 6 && !Q_stricmp(base + n - 6, "_light")) {
        n -= 6;
        base[n] = 0;
        light = true;
    }

    // a rerelease skin, its own file
    if (Q_concat(path, sizeof(path), base, ".lmp") < sizeof(path)) {
        byte *data = NULL;

        len = FS_LoadFile(path, (void **)&data);
        if (data) {
            if (len >= 8) {
                int lw = (int)LittleLong(*(uint32_t *)data);
                int lh = (int)LittleLong(*(uint32_t *)(data + 4));
                if (lw > 0 && lh > 0 && lw <= 4096 && lh <= 4096 && (size_t)len >= 8 + (size_t)lw * lh) {
                    src = Z_Malloc((size_t)lw * lh);
                    memcpy(src, data + 8, (size_t)lw * lh);
                    w = lw;
                    h = lh;
                }
            }
            FS_FreeFile(data);
        }
    }

    // an .mdl's skin: <model>_mdl_<skin>_<frame>
    if (!src && (tag = q1_name_tag(base, "_mdl_", 2, numbers)) != NULL) {
        if (Q_snprintf(path, sizeof(path), "%.*s.mdl", (int)(tag - base), base) < sizeof(path))
            src = MOD_Q1ModelSkin(path, numbers[0], numbers[1], &w, &h);
    }

    // a sprite's frame: <sprite>_spr_<frame>
    if (!src && (tag = q1_name_tag(base, "_spr_", 1, numbers)) != NULL) {
        if (Q_snprintf(path, sizeof(path), "%.*s.spr", (int)(tag - base), base) < sizeof(path)) {
            src = MOD_Q1SpriteFrame(path, numbers[0], &w, &h);
            fence = true;
        }
    }

    if (!src)
        return Q_ERR(ENOENT);

    out = q1_expand_pixels(src, w, h, light, fence, &alpha);
    Z_Free(src);
    if (!out)
        return Q_ERR(ENOENT);   // nothing in it glows

    q1_finish_image(image, type, flags, w, h, alpha);
    *pic = (byte *)out;
    return Q_ERR_SUCCESS;
}

#define Q1_WAD_QPIC     0x42    // 'B': a qpic, its width and height first
#define Q1_WAD_RAW      0x44    // 'D': conchars, 128x128 with no header

// the gfx.wad the last picture came from: the status bar's are all in one
static char     q1_wad_path[MAX_QPATH];
static byte     *q1_wad_data;
static int      q1_wad_len;

static void q1_free_wad(void)
{
    if (q1_wad_data)
        FS_FreeFile(q1_wad_data);
    q1_wad_data = NULL;
    q1_wad_len = 0;
    q1_wad_path[0] = 0;
}

// lump (name) of the WAD2 at (path): its type and size, or NULL
static const byte *q1_wad_lump(const char *path, const char *name, int *type, int *size)
{
    if (Q_stricmp(path, q1_wad_path)) {
        q1_free_wad();
        q1_wad_len = FS_LoadFile(path, (void **)&q1_wad_data);
        Q_strlcpy(q1_wad_path, path, sizeof(q1_wad_path));     // a missing one too
    }
    if (!q1_wad_data || q1_wad_len < 12 || memcmp(q1_wad_data, "WAD2", 4))
        return NULL;

    const int numlumps = (int)LittleLong(*(uint32_t *)(q1_wad_data + 4));
    const int infotable = (int)LittleLong(*(uint32_t *)(q1_wad_data + 8));
    if (numlumps < 0 || infotable < 12 || (int64_t)infotable + (int64_t)numlumps * 32 > q1_wad_len)
        return NULL;

    for (int i = 0; i < numlumps; i++) {
        // filepos, disksize, size, type, compression, 2 pad, name[16]
        const byte *l = q1_wad_data + infotable + i * 32;
        char lname[17];

        memcpy(lname, l + 16, 16);
        lname[16] = 0;
        if (Q_stricmp(lname, name))
            continue;

        const int filepos = (int)LittleLong(*(uint32_t *)l);
        const int disksize = (int)LittleLong(*(uint32_t *)(l + 4));
        if (l[13] || filepos < 0 || disksize < 0 || (int64_t)filepos + disksize > q1_wad_len)
            return NULL;    // compressed (never was), or cut short
        *type = l[12];
        *size = disksize;
        return q1_wad_data + filepos;
    }
    return NULL;
}

/*
=================
load_q1_pic

find_or_load_image's last resort for a Quake 1 picture, "id1/<episode>/gfx/
<name>.lmp": the status bar's and the intermission's (screen.c's Quake 1
status bar). That file when there is one - a qpic: a width and a height, then
that many palette indices - and otherwise lump <name> of the episode's gfx.wad,
which the file system takes from id1 when the episode has none of its own.
Index 255 is see-through, as Quake drew them with Draw_TransPic; in "conchars",
the 128x128 font with no header, index 0 is.
=================
*/
static int load_q1_pic(image_t *image, imagetype_t type, imageflags_t flags, byte **pic)
{
    char base[MAX_QPATH], path[MAX_QPATH];
    byte *src = NULL, *data = NULL;
    const char *slash;
    bool alpha;
    int w = 0, h = 0, len;
    const size_t n = image->baselen;

    if ((type != IT_PIC && type != IT_FONT) || n >= sizeof(base))
        return Q_ERR(ENOENT);
    if (Q_strncasecmp(image->name, "id1/", 4) || Q_stricmp(image->name + n, ".lmp"))
        return Q_ERR(ENOENT);
    if (!q1_load_palette())
        return Q_ERR(ENOENT);

    memcpy(base, image->name, n);
    base[n] = 0;
    slash = strrchr(base, '/');
    if (!slash || slash - base < 4 || Q_strncasecmp(slash - 4, "/gfx", 4))
        return Q_ERR(ENOENT);

    // its own file
    len = FS_LoadFile(image->name, (void **)&data);
    if (data) {
        if (len >= 8) {
            const int lw = (int)LittleLong(*(uint32_t *)data);
            const int lh = (int)LittleLong(*(uint32_t *)(data + 4));
            if (lw > 0 && lh > 0 && lw <= 4096 && lh <= 4096 && (size_t)len >= 8 + (size_t)lw * lh) {
                src = Z_Malloc((size_t)lw * lh);
                memcpy(src, data + 8, (size_t)lw * lh);
                w = lw;
                h = lh;
            }
        }
        FS_FreeFile(data);
    }

    // a lump of the gfx.wad beside the gfx directory
    if (!src && Q_snprintf(path, sizeof(path), "%.*sgfx.wad", (int)(slash - base - 3), base) < sizeof(path)) {
        int lumptype = 0, size = 0;
        const byte *lump = q1_wad_lump(path, slash + 1, &lumptype, &size);

        if (lump && lumptype == Q1_WAD_QPIC && size >= 8) {
            const int lw = (int)LittleLong(*(uint32_t *)lump);
            const int lh = (int)LittleLong(*(uint32_t *)(lump + 4));
            if (lw > 0 && lh > 0 && lw <= 4096 && lh <= 4096 && (size_t)size >= 8 + (size_t)lw * lh) {
                src = Z_Malloc((size_t)lw * lh);
                memcpy(src, lump + 8, (size_t)lw * lh);
                w = lw;
                h = lh;
            }
        } else if (lump && lumptype == Q1_WAD_RAW && size == 128 * 128) {
            src = Z_Malloc(128 * 128);
            for (int i = 0; i < 128 * 128; i++)
                src[i] = lump[i] ? lump[i] : 255;   // Draw_Init: 0 is see-through
            w = h = 128;
        }
    }

    if (!src)
        return Q_ERR(ENOENT);

    uint32_t *out = q1_expand_pixels(src, w, h, false, true, &alpha);
    Z_Free(src);

    q1_finish_image(image, type, flags, w, h, alpha);
    *pic = (byte *)out;
    return Q_ERR_SUCCESS;
}

static int get_8bit_dimensions(const char* name, int* width, int* height)
{
    assert(name);
    assert(width);
    assert(height);

    int w = 0;
    int h = 0;

    ssize_t len = strlen(name);
    if (len <= 4)
        return Q_ERR_NAMETOOSHORT;

    imageformat_t format;
    if (Q_stricmp(name + len - 4, ".wal") == 0)
        format = IM_WAL;
    else if (Q_stricmp(name + len - 4, ".pcx") == 0)
        format = IM_PCX;
    else
        return Q_ERR_INVALID_FORMAT;

    qhandle_t f;
    FS_OpenFile(name, &f, FS_MODE_READ);
    if (!f) {
        // a Quake 1 map's own texture: its size is the one its faces are mapped in
        bool light, fence;
        int sky_layer;
        if (format == IM_WAL && q1_find_texture(name, len - 4, width, height, &light, &fence, &sky_layer) && !light) {
            if (sky_layer >= 0)
                *width /= 2;
            return Q_ERR_SUCCESS;
        }
        return Q_ERR(ENOENT);
    }

    if (format == IM_WAL)
    {
        miptex_t mt;
        len = FS_Read(&mt, sizeof(mt), f);
        if (len == sizeof(mt)) {
            w = LittleLong(mt.width);
            h = LittleLong(mt.height);
        }
    }
    else if (format == IM_PCX)
    {
        dpcx_t pcx;
        len = FS_Read(&pcx, sizeof(pcx), f);
        if (len == sizeof(pcx)) {
            w = (LittleShort(pcx.xmax) - LittleShort(pcx.xmin)) + 1;
            h = (LittleShort(pcx.ymax) - LittleShort(pcx.ymin)) + 1;
        }
    }

    FS_CloseFile(f);

    if (w < 1 || h < 1 || w > 512 || h > 512) {
        return Q_ERR_INVALID_FORMAT;
    }

    *width = w;
    *height = h;

    return Q_ERR_SUCCESS;
}

/*
The size of the image at (name)'s path in whatever format is there, for an
original that has no .wal or .pcx to read it from. The rerelease's N64 textures
are like that: textures/n64/block1_6 is a 32x32 .tga and nothing else. A full
load, since the formats keep their sizes in different places - but only ever of
an original that an override is standing in for, and those are 16..64 pixels.
*/
static int get_32bit_dimensions(const char *name, int *width, int *height)
{
    image_t temp;
    byte    *pic = NULL;
    size_t  len = strlen(name);
    int     ret;

    if (len <= 4 || len >= sizeof(temp.name))
        return Q_ERR_INVALID_PATH;

    memset(&temp, 0, sizeof(temp));
    memcpy(temp.name, name, len + 1);
    temp.baselen = len - 4;
    temp.type = IT_WALL;

    ret = try_other_formats(IM_MAX, &temp, TRY_IMAGE_SRC_BASE, &pic);
    if (pic)
        IMG_FreePixels(pic);
    if (ret < 0)
        return ret;
    if (temp.upload_width < 1 || temp.upload_height < 1)
        return Q_ERR_INVALID_FORMAT;

    *width = temp.upload_width;
    *height = temp.upload_height;
    return Q_ERR_SUCCESS;
}

// The original size of the texture (name) - its .wal or .pcx, or failing that
// the 32-bit image at the same path.
int IMG_GetDimensions(const char* name, int* width, int* height)
{
    int ret = get_8bit_dimensions(name, width, height);

    if (ret == Q_ERR(ENOENT))
        ret = get_32bit_dimensions(name, width, height);
    return ret;
}

// .wal/.pcx only: an image loaded under its own name already has the size of
// the 32-bit file it came from, and reading that again would be a second load.
// An override is the case that needs more - see override_fits_original.
static int get_image_dimensions(imageformat_t fmt, image_t *image)
{
    char buffer[MAX_IMAGE_PATH];
    memcpy(buffer, image->name, image->baselen + 1);

    if (fmt == IM_WAL) {
        memcpy(buffer + image->baselen + 1, "wal", 4);
    } else {
        memcpy(buffer + image->baselen + 1, "pcx", 4);
    }

    return get_8bit_dimensions(buffer, &image->width, &image->height);
}

/*
=================
override_fits_original

THE N64 EPISODE'S TEXTURES HAVE NO .WAL. An override replaces a texture by its
base name alone (overrides/block1_6.tga for a block1_6 in any directory), and
the size the map's texture coordinates are measured in is then recovered from
the .wal or .pcx it stands in for (get_image_dimensions). The N64 maps' textures
are only
32-bit - textures/n64/block1_6.tga, 32x32 - so nothing was recovered and the
override's own size stood in: a 256x256 replacement drew 8x too big, and every
face against one that had no override drew at the right size, out of line with
it. Matt saw that as the textures being "zoomed in" and "badly aligned". 159 of
the episode's 292 textures, 47k of its 74k faces.

So when a wall's override has no 8-bit original to measure, it is measured
against the 32-bit one instead:

  - The same shape: the original's size is the one the coordinates are in.
  - A different shape: it is a different picture, and is refused - the N64
    maps reuse PC names for their own art, and n64/door1_1 (32x32) is not the
    PC door1_1 (640x256), which squeezed into a square would be no door at all.
    The original is drawn instead.

Sidecars follow their base: <base>_n, _rough, _metallic, _height, _light and
_glow are measured against the BASE's original, so a refused texture does not
keep the PC normal map of the picture that was refused.

A texture with a .wal or .pcx - every PC texture, every MGU one - is never
touched here: its override was already measured against that, and is kept.
Neither is one with no original at all.
=================
*/
static bool override_fits_original(image_t *image, imagetype_t type)
{
    static const char *const sidecars[] = {
        "_n", "_rough", "_metallic", "_height", "_light", "_glow",
    };
    char    base[MAX_IMAGE_PATH];
    size_t  len;
    bool    is_sidecar = false;
    int     w, h, ret;

    if (type != IT_WALL || image->baselen >= sizeof(base) - 4)
        return true;

    memcpy(base, image->name, image->baselen);
    len = image->baselen;
    base[len] = 0;

    for (int i = 0; i < q_countof(sidecars); i++) {
        size_t n = strlen(sidecars[i]);

        if (len > n && !Q_stricmp(base + len - n, sidecars[i])) {
            len -= n;
            is_sidecar = true;
            break;
        }
    }

    // the 8-bit original, if any, decides - and it already has
    memcpy(base + len, ".wal", 5);
    ret = get_8bit_dimensions(base, &w, &h);
    if (ret != Q_ERR(ENOENT))
        return true;
    memcpy(base + len, ".pcx", 5);
    ret = get_8bit_dimensions(base, &w, &h);
    if (ret != Q_ERR(ENOENT))
        return true;

    memcpy(base + len, ".tga", 5);
    if (get_32bit_dimensions(base, &w, &h) < 0)
        return true;            // no original at all: nothing to measure against

    // aspect, cross-multiplied, with a little slack for a replacement that
    // was resized to a not-quite-proportional size
    if (fabsf((float)w * image->upload_height - (float)h * image->upload_width) >
        0.02f * (float)w * image->upload_height)
        return false;

    if (!is_sidecar) {
        image->width = w;
        image->height = h;
    }
    return true;
}

static void r_texture_formats_changed(cvar_t *self)
{
    char *s;
    int i, j;

    // reset the search order
    img_total = 0;

    // parse the string
    for (s = self->string; *s; s++) {
        switch (*s) {
            case 't': case 'T': i = IM_TGA; break;
            case 'j': case 'J': i = IM_JPG; break;
            case 'p': case 'P': i = IM_PNG; break;
            default: continue;
        }

        // don't let format to be specified more than once
        for (j = 0; j < img_total; j++)
            if (img_search[j] == i)
                break;
        if (j != img_total)
            continue;

        img_search[img_total++] = i;
        if (img_total == IM_MAX) {
            break;
        }
    }
}

int
load_img(const char *name, image_t *image)
{
    byte            *pic;
    imageformat_t   fmt;
    int             ret = Q_ERR(EINVAL);

	size_t len = strlen(name);

    // must have an extension and at least 1 char of base name
    if (len <= 4) {
        return Q_ERR_NAMETOOSHORT;
    }
    if (name[len - 4] != '.') {
        return Q_ERR_INVALID_PATH;
    }

    memcpy(image->name, name, len + 1);
    image->baselen = len - 4;
    image->type = 0;
    image->flags = 0;
    image->registration_sequence = 1;

    // find out original extension
    for (fmt = 0; fmt < IM_MAX; fmt++) {
        if (!Q_stricmp(image->name + image->baselen + 1, img_loaders[fmt].ext)) {
            break;
        }
    }

    // load the pic from disk
    pic = NULL;

    // Always prefer images from the game dir, even if format might be 'inferior'
    for (int try_location = Q_stricmp(fs_game->string, BASEGAME) ? TRY_IMAGE_SRC_GAME : TRY_IMAGE_SRC_BASE;
         try_location >= TRY_IMAGE_SRC_BASE;
         try_location--)
    {
        int location_flag = try_location == TRY_IMAGE_SRC_GAME ? IF_SRC_GAME : IF_SRC_MASK;
        if(((image->flags & IF_SRC_MASK) != 0) && ((image->flags & IF_SRC_MASK) != location_flag))
            continue;

        // first try with original extension
        ret = _try_image_format(fmt, image, try_location, &pic);
        if (ret == Q_ERR(ENOENT)) {
            // retry with remaining extensions
            ret = try_other_formats(fmt, image, try_location, &pic);
        }
        if (ret >= 0)
            break;
    }

    // if we are replacing 8-bit texture with a higher resolution 32-bit
    // texture, we need to recover original image dimensions
    if (fmt <= IM_WAL && ret > IM_WAL) {
        get_image_dimensions(fmt, image);
    }

    if (ret < 0) {
        memset(image, 0, sizeof(*image));
        return ret;
    }

#if USE_REF == REF_VKPT
	image->pix_data = pic;
#endif

    return Q_ERR_SUCCESS;
}

/* load_img for the file at the OS path (ospath) itself, not looked up along the
   search path; (name) is the game path it stands for - its extension picks the
   loader, and it becomes the image's name and filepath. For the texture reload
   (IMG_ReloadFrom), when a read through the search path does not give back the
   file whose time changed. */
int load_img_from_disk(const char *ospath, const char *name, image_t *image)
{
    size_t          len = strlen(name);
    imageformat_t   fmt;
    FILE            *fp;
    byte            *data, *pic = NULL;
    long            size;
    int             ret;

    memset(image, 0, sizeof(*image));
    if (len <= 4 || len >= sizeof(image->name) || name[len - 4] != '.')
        return Q_ERR_INVALID_PATH;
    for (fmt = 0; fmt < IM_MAX; fmt++)
        if (!Q_stricmp(name + len - 3, img_loaders[fmt].ext))
            break;
    if (fmt == IM_MAX)
        return Q_ERR_INVALID_FORMAT;

    fp = Q_fopen(ospath, "rb");
    if (!fp)
        return Q_ERRNO;
    if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
        ret = Q_ERRNO;
        fclose(fp);
        return ret;
    }
    if (size == 0 || size > MAX_LOADFILE) {
        fclose(fp);
        return size ? Q_ERR(EFBIG) : Q_ERR_UNEXPECTED_EOF;
    }
    data = Z_Malloc(size);
    if (fread(data, 1, size, fp) != (size_t)size) {
        Z_Free(data);
        fclose(fp);
        return Q_ERR_UNEXPECTED_EOF;
    }
    fclose(fp);

    memcpy(image->name, name, len + 1);
    image->baselen = len - 4;
    image->registration_sequence = 1;
    ret = img_loaders[fmt].load(data, size, image, &pic);
    Z_Free(data);
    if (ret < 0) {
        memset(image, 0, sizeof(*image));
        return ret;
    }
    strcpy(image->filepath, image->name);

#if USE_REF == REF_VKPT
    image->pix_data = pic;
#else
    IMG_FreePixels(pic);
#endif

    return Q_ERR_SUCCESS;
}

// Try to load an image, possibly with an alternative extension
static int try_load_image_candidate(image_t *image, const char *orig_name, size_t orig_len, byte **pic_p, imagetype_t type, imageflags_t flags, bool ignore_extension, int try_location)
{
    int ret;

    image->type = type;
    image->flags = flags;
    image->registration_sequence = registration_sequence;

    // find out original extension
    imageformat_t fmt;
    for (fmt = 0; fmt < IM_MAX; fmt++)
    {
        if (!Q_stricmp(image->name + image->baselen + 1, img_loaders[fmt].ext))
        {
            break;
        }
    }

    // load the pic from disk
    *pic_p = NULL;

    if (fmt == IM_MAX)
    {
        // unknown extension, but give it a chance to load anyway
        ret = try_other_formats(IM_MAX, image, try_location, pic_p);
        if (ret == Q_ERR(ENOENT))
        {
            // not found, change error to invalid path
            ret = Q_ERR_INVALID_PATH;
        }
    }
    else if (ignore_extension)
    {
        // forcibly replace the extension
        ret = try_other_formats(IM_MAX, image, try_location, pic_p);
    }
    else
    {
        // first try with original extension
        ret = _try_image_format(fmt, image, try_location, pic_p);
        if (ret == Q_ERR(ENOENT) && !(flags & IF_EXACT))
        {
            // retry with remaining extensions
            ret = try_other_formats(fmt, image, try_location, pic_p);
        }
    }

    // record last modified time (skips reload when invoking IMG_ReloadAll)
    image->last_modified = 0;
    FS_LastModified(image->name, &image->last_modified);

    // Restore original name if it was overridden
    if(orig_name) {
        memcpy(image->name, orig_name, orig_len + 1);
        image->baselen = orig_len - 4;
    }

    // if we are replacing 8-bit texture with a higher resolution 32-bit
    // texture, we need to recover original image dimensions
    if (fmt <= IM_WAL && ret > IM_WAL)
    {
        get_image_dimensions(fmt, image);
    }

    return ret;
}

// finds or loads the given image, adding it to the hash table.
// exact_file: IMG_FindFile's lookup - an image of the same name read from a
// file of another format is not this one, and a new image is loaded beside it
static int find_or_load_image(const char *name, size_t len,
                                   imagetype_t type, imageflags_t flags,
                                   bool exact_file, image_t **image_p)
{
    image_t         *image;
    byte            *pic;
    unsigned        hash;
    int             ret = Q_ERR(ENOENT);

    *image_p = NULL;

    // must have an extension and at least 1 char of base name
    if (len <= 4) {
        return Q_ERR_NAMETOOSHORT;
    }
    if (name[len - 4] != '.') {
        return Q_ERR_INVALID_PATH;
    }

    hash = FS_HashPathLen(name, len - 4, RIMAGES_HASH);

    // look for it
    image = exact_file ? lookup_image_file(name, type, hash, len - 4)
                       : lookup_image(name, type, hash, len - 4);
    if (image != NULL) {
        image->registration_sequence = registration_sequence;
        if (image->upload_width && image->upload_height) {
            image->flags |= flags & IF_PERMANENT;
            *image_p = image;
            return Q_ERR_SUCCESS;
        }
        // placeholder for a temp pic that previously failed to load
        return Q_ERR(ENOENT);
    }

    // allocate image slot
    image = alloc_image();
    if (!image) {
        return Q_ERR_OUT_OF_SLOTS;
    }

	int override_textures = !!r_override_textures->integer;
	if (type != IT_PIC && classic_assets())
		override_textures = 0;
    if (flags & IF_EXACT)
        override_textures = 0;
    /* NO QUAKE II OVERRIDE FOR A QUAKE 1 TEXTURE. overrides/ is matched by base
       name alone, and Q2RTX's 2019 set carries 174 names Quake 1 maps use too -
       Quake II reused id's names for its own art. So start's episode 3 hall
       (q1/metal5_4) drew Quake II's e1u2 metal5_4 with its normal map,
       and every q1 texture without a normal map of its own took the namesake's
       _n / _light (Matt: "these quake2 textures don't fit at all", 2026-11-21).
       A Quake 1 map that wants one names the file in a .mat - id1/maps/q1/
       e1m1.mat keeps e1m1's computer panels on theirs. A loose textures/q1/
       <name>.png still wins as before. */
    if (!Q_strncasecmp(name, Q1_TEXTURE_PREFIX, Q1_TEXTURE_PREFIX_LEN))
        override_textures = 0;

    if(override_textures)
    {
        const char *last_slash = strrchr(name, '/');
        if (!last_slash)
            last_slash = name;
        else
            last_slash += 1;

        // a long name has no room for the prefix: it has no override
        if (Q_concat(image->name, sizeof(image->name), "overrides/", last_slash) < sizeof(image->name)) {
            image->baselen = strlen(image->name) - 4;
            ret = try_load_image_candidate(image, name, len, &pic, type, flags, true, -1);
        }
        memcpy(image->name, name, len + 1);
        image->baselen = len - 4;

        // a different picture that only shares the name - the original is
        // loaded below instead
        if (ret >= 0 && !override_fits_original(image, type)) {
            if (pic)
                IMG_FreePixels(pic);
            pic = NULL;
            ret = Q_ERR(ENOENT);
        }
    }

    /* GL WITHOUT "use new textures and models" draws the classic .md2 and .wal,
       so take the image in its original format first, from any directory. The
       game-dir-first search below otherwise finds a replacement made for a
       DIFFERENT model before it reaches baseq2's original: rerelease/models/
       weapons/v_shotg/skin.tga belongs to Q2RTX's tris.md3 there and comes
       out as camouflage on the md2's UVs. A substitute is still taken when
       there is no original at all. */
    if (ret < 0 && type != IT_PIC && classic_assets() &&
        !(flags & (IF_EXACT | IF_SRC_MASK)))
    {
        memcpy(image->name, name, len + 1);
        image->baselen = len - 4;
        ret = try_load_image_candidate(image, NULL, 0, &pic, type, flags | IF_EXACT, false, TRY_IMAGE_SRC_BASE);
        image->flags &= ~IF_EXACT;
    }

    // Try non-overridden image
    if (ret < 0)
    {
        bool is_not_baseq2 = fs_game->string[0] && strcmp(fs_game->string, BASEGAME) != 0;

        if (is_not_baseq2 && !(flags & IF_SRC_MASK) && prefer_base_pic(name, len - 4)) {
            memcpy(image->name, name, len + 1);
            image->baselen = len - 4;
            ret = try_load_image_candidate(image, NULL, 0, &pic, type, flags, !!override_textures, TRY_IMAGE_SRC_BASE_ONLY);
            image->flags |= IF_SRC_BASE;
        }
    	
        // Always prefer images from the game dir, even if format might be 'inferior'
        for (int try_location = is_not_baseq2 ? TRY_IMAGE_SRC_GAME : TRY_IMAGE_SRC_BASE;
            try_location >= TRY_IMAGE_SRC_BASE;
            try_location--)
        {
            int location_flag = try_location == TRY_IMAGE_SRC_GAME ? IF_SRC_GAME : IF_SRC_BASE;
            if (ret >= 0)
                break;  // a prefer_base_pics entry already loaded
            if(((flags & IF_SRC_MASK) != 0) && ((flags & IF_SRC_MASK) != location_flag))
                continue;

            // fill in some basic info
            memcpy(image->name, name, len + 1);
            image->baselen = len - 4;
            ret = try_load_image_candidate(image, NULL, 0, &pic, type, flags, !!override_textures, try_location);
            image->flags |= location_flag;

            if (ret >= 0)
                break;
        }
    }

    // a Quake 1 map's texture with no file of its own: from the map - or, on
    // a health or ammo box's model, from the box (model_q1.c)
    if (ret < 0 && (type == IT_WALL || type == IT_SKIN) && !(flags & IF_EXACT)) {
        memcpy(image->name, name, len + 1);
        image->baselen = len - 4;
        pic = NULL;
        ret = load_q1_texture(image, type, flags, &pic);
    }

    // a Quake 1 model's skin or sprite frame: from the model
    if (ret < 0 && (type == IT_SKIN || type == IT_SPRITE) && !(flags & IF_EXACT)) {
        memcpy(image->name, name, len + 1);
        image->baselen = len - 4;
        pic = NULL;
        ret = load_q1_model_image(image, type, flags, &pic);
    }

    // a Quake 1 picture: its .lmp, or its lump of gfx.wad
    if (ret < 0 && (type == IT_PIC || type == IT_FONT)) {
        memcpy(image->name, name, len + 1);
        image->baselen = len - 4;
        pic = NULL;
        ret = load_q1_pic(image, type, flags, &pic);
    }

    if (ret < 0) {
        memset(image, 0, sizeof(*image));
        if (type == IT_PIC && !(flags & IF_PERMANENT)) {
            // don't reload missing temp pics (HUD icons etc) from disk every
            // frame: keep an empty placeholder in the hash table until the
            // next registration sequence frees it
            memcpy(image->name, name, len + 1);
            image->baselen = len - 4;
            image->type = type;
            image->flags = flags;
            image->registration_sequence = registration_sequence;
            List_Append(&r_imageHash[hash], &image->entry);
        }
        return ret;
    }

    List_Append(&r_imageHash[hash], &image->entry);

	image->is_srgb = !!(flags & IF_SRGB);

    // upload the image
    IMG_Load(image, pic);

    *image_p = image;
    return Q_ERR_SUCCESS;
}

image_t *IMG_Find(const char *name, imagetype_t type, imageflags_t flags)
{
    image_t *image;
    size_t len;
    int ret;

    Q_assert(name);

    len = strlen(name);
    if (len >= MAX_IMAGE_PATH) {
        Com_EPrintf("Couldn't load %s: the path is %u characters, and can be %d at most\n",
                    name, (unsigned)len, MAX_IMAGE_PATH - 1);
        return R_NOTEXTURE;
    }

    ret = find_or_load_image(name, len, type, flags, false, &image);
    if (image) {
        return image;
    }

    // don't spam about missing images
    if (ret != Q_ERR(ENOENT)) {
        Com_EPrintf("Couldn't load %s: %s\n", name, Q_ErrorString(ret));
    }

    return R_NOTEXTURE;
}

/* The image of EXACTLY this file. IMG_Find matches the images already loaded by
   name without the extension, IF_EXACT or not, so it hands back foo.jpg for
   foo.png when foo.jpg came first. Here the file named is the one read: an image
   of the same name from another format stays as it is for whoever asked for
   that, and this one is loaded beside it. A file that is not there gets what
   IMG_Find makes of the name. For paths a .mat or the console names on purpose -
   Quake III stage maps, and the live texture edits - not the classic lookups,
   whose override formats depend on sharing one image per name. */
image_t *IMG_FindFile(const char *name, imagetype_t type, imageflags_t flags)
{
    image_t *image;
    size_t len;
    int ret;

    Q_assert(name);

    len = strlen(name);
    if (len >= MAX_IMAGE_PATH || !FS_FileExists(name)) {
        return IMG_Find(name, type, flags | IF_EXACT);
    }

    ret = find_or_load_image(name, len, type, flags | IF_EXACT, true, &image);
    if (image) {
        return image;
    }

    if (ret != Q_ERR(ENOENT)) {
        Com_EPrintf("Couldn't load %s: %s\n", name, Q_ErrorString(ret));
    }

    return R_NOTEXTURE;
}

image_t *IMG_FindExisting(const char *name, imagetype_t type)
{
    image_t *image;
    size_t len;
    unsigned hash;

    if (!name) {
        Com_Error(ERR_FATAL, "%s: NULL", __func__);
    }

    // this should never happen
    len = strlen(name);
    if (len >= MAX_IMAGE_PATH) {
        Com_Error(ERR_FATAL, "%s: oversize name", __func__);
    }

    // must have an extension and at least 1 char of base name
    if (len <= 4) {
        return R_NOTEXTURE;
    }
    if (name[len - 4] != '.') {
        return R_NOTEXTURE;
    }

    hash = FS_HashPathLen(name, len - 4, RIMAGES_HASH);

    // look for it
    if ((image = lookup_image(name, type, hash, len - 4)) != NULL) {
        return image;
    }

    return R_NOTEXTURE;
}

/*
===============
IMG_Clone
===============
*/
image_t *IMG_Clone(image_t *image, const char* new_name)
{
    if(image == R_NOTEXTURE)
        return image;

    image_t* new_image = alloc_image();
    if (!new_image)
        return R_NOTEXTURE;

    memcpy(new_image, image, sizeof(image_t));

#if USE_REF == REF_VKPT
    // the upscale belongs to the materials that draw an image (material.c
    // MAT_SyncUploadScales): a copy starts as loaded until one of them asks
    new_image->upload_scale = 0;
    size_t image_size = image->upload_width * image->upload_height * 4;
    if(image->pix_data != NULL)
    {
        new_image->pix_data = IMG_AllocPixels(image_size);
        memcpy(new_image->pix_data, image->pix_data, image_size);
    }
#else
    for (int m = 0; m < 4; m++)
    {
        if(image->pixels[m] != NULL)
        {
            size_t mip_size = (image->upload_width >> m) * (image->upload_height >> m) * 4;
            new_image->pixels[m] = IMG_AllocPixels(mip_size);
            memcpy(new_image->pixels[m], image->pixels[m], mip_size);
        }
    }
#endif

    if(new_name)
    {
        Q_strlcpy(new_image->name, new_name, sizeof(new_image->name));
        new_image->baselen = strlen(new_image->name) - 4;
        assert(new_image->name[new_image->baselen] == '.');
    }
    unsigned hash = FS_HashPathLen(new_image->name, new_image->baselen, RIMAGES_HASH);
    List_Append(&r_imageHash[hash], &new_image->entry);
    return new_image;
}

/*
===============
IMG_ForHandle
===============
*/
image_t *IMG_ForHandle(qhandle_t h)
{
    Q_assert(h >= 0 && h < r_numImages);
    return &r_images[h];
}

/*
===============
R_RegisterImage
===============
*/
qhandle_t R_RegisterImage(const char *name, imagetype_t type,
                          imageflags_t flags, int *err_p)
{
    image_t     *image;
    char        fullname[MAX_QPATH];
    size_t      len;
    int         err;

    // empty names are legal, silently ignore them
    if (!*name) {
        if (err_p)
            *err_p = Q_ERR_NAMETOOSHORT;
        return 0;
    }

    // no images = not initialized
    if (!r_numImages) {
        if (err_p)
            *err_p = Q_ERR(EAGAIN);
        return 0;
    }

    if (type == IT_SKIN) {
        len = FS_NormalizePathBuffer(fullname, name, sizeof(fullname));
    } else if (*name == '/' || *name == '\\') {
        len = FS_NormalizePathBuffer(fullname, name + 1, sizeof(fullname));
    } else {
        len = Q_concat(fullname, sizeof(fullname), "pics/", name);
        if (len < sizeof(fullname)) {
            FS_NormalizePath(fullname);
            len = COM_DefaultExtension(fullname, ".pcx", sizeof(fullname));
        }
    }

    if (len >= sizeof(fullname)) {
        err = Q_ERR(ENAMETOOLONG);
        goto fail;
    }

    err = find_or_load_image(fullname, len, type, flags, false, &image);
    if (image) {
        if (err_p)
            *err_p = Q_ERR_SUCCESS;
        return image - r_images;
    }

fail:
    // don't spam about missing images
    if (err_p)
        *err_p = err;
    else if (err != Q_ERR(ENOENT))
        Com_EPrintf("Couldn't load %s: %s\n", fullname, Q_ErrorString(err));

    return 0;
}

qhandle_t R_RegisterRawImage(const char *name, int width, int height, byte* pic, imagetype_t type, imageflags_t flags)
{
    image_t         *image;
    unsigned        hash;

    int len = strlen(name);
    hash = FS_HashPathLen(name, len, RIMAGES_HASH);

    // look for it
    if ((image = lookup_image(name, type, hash, len)) != NULL) {
        image->flags |= flags & IF_PERMANENT;
        image->registration_sequence = registration_sequence;
        return image - r_images;
    }

    // allocate image slot
    image = alloc_image();
    if (!image) {
        return 0;
    }

    memcpy(image->name, name, len + 1);
    image->baselen = len;
    image->type = type;
    image->flags = flags;
    image->registration_sequence = registration_sequence;
    image->last_modified = 0;
    image->width = width;
    image->height = height;
    image->upload_width = width;
    image->upload_height = height;

    List_Append(&r_imageHash[hash], &image->entry);

    image->is_srgb = !!(flags & IF_SRGB);

    // upload the image
    IMG_Load(image, pic);

    return image - r_images;
}

void R_UnregisterImage(qhandle_t handle)
{
    if (!handle)
        return;

    image_t* image = r_images + handle;

    if (image->registration_sequence)
    {
        image->registration_sequence = -1;
        IMG_FreeUnused();
    }
}

/*
=============
R_GetPicSize
=============
*/
bool R_GetPicSize(int *w, int *h, qhandle_t pic)
{
    image_t *image = IMG_ForHandle(pic);

    if (w) {
        *w = image->width;
    }
    if (h) {
        *h = image->height;
    }
    return image->flags & IF_TRANSPARENT;
}

/*
================
IMG_FreeUnused

Any image that was not touched on this registration sequence
will be freed.
================
*/
void IMG_FreeUnused(void)
{
    image_t *image;
    int i, count = 0;

    for (i = 1, image = r_images + 1; i < r_numImages; i++, image++) {
        if (image->registration_sequence == registration_sequence) {
            continue;        // used this sequence
        }
        if (!image->registration_sequence)
            continue;        // free image_t slot
        if (image->flags & (IF_PERMANENT | IF_SCRAP))
            continue;        // don't free pics

        // delete it from hash table
        List_Remove(&image->entry);

        // free it
        IMG_Unload(image);

        memset(image, 0, sizeof(*image));
        count++;
    }

    if (count) {
        Com_DPrintf("%s: %i images freed\n", __func__, count);
    }
}

void IMG_FreeAll(void)
{
    image_t *image;
    int i, count = 0;

    for (i = 1, image = r_images + 1; i < r_numImages; i++, image++) {
        if (!image->registration_sequence)
            continue;        // free image_t slot
        // free it
        IMG_Unload(image);

        memset(image, 0, sizeof(*image));
        count++;
    }

    if (count) {
        Com_DPrintf("%s: %i images freed\n", __func__, count);
    }

    for (i = 0; i < RIMAGES_HASH; i++) {
        List_Init(&r_imageHash[i]);
    }

    q1_free_wad();

    // &r_images[0] == R_NOTEXTURE
    r_numImages = 1;
}

/*
===============
R_GetPalette

===============
*/
void IMG_GetPalette(void)
{
    byte        pal[768], *src, *data;
    int         i, ret, len;

    // get the palette
    len = FS_LoadFile(R_COLORMAP_PCX, (void **)&data);
    if (!data) {
        ret = len;
        goto fail;
    }

    ret = IMG_DecodePCX(data, len, NULL, pal, NULL, NULL);

    FS_FreeFile(data);

    if (ret < 0) {
        goto fail;
    }

    for (i = 0, src = pal; i < 255; i++, src += 3) {
        d_8to24table[i] = MakeColor(src[0], src[1], src[2], 255);
    }

    // 255 is transparent
    d_8to24table[i] = MakeColor(src[0], src[1], src[2], 0);
    return;

fail:
    Com_Error(ERR_FATAL, "Couldn't load %s: %s", R_COLORMAP_PCX, Q_ErrorString(ret));
}

/*
===============
IMG_LoadColormap

The pixels of colormap.pcx for the software renderer's 8-bit mode: 64 rows
of light levels, then the 256 rows of the translucency table. `pixels` holds
256 x 320.
===============
*/
int IMG_LoadColormap(byte *pixels)
{
    byte    *data;
    int     ret, len, w, h;

    len = FS_LoadFile(R_COLORMAP_PCX, (void **)&data);
    if (!data)
        return len;

    // decode the header first: the pixels must fit
    ret = IMG_DecodePCX(data, len, NULL, NULL, &w, &h);
    if (ret >= 0 && (w != 256 || h != 320))
        ret = Q_ERR_INVALID_FORMAT;
    if (ret >= 0)
        ret = IMG_DecodePCX(data, len, pixels, NULL, NULL, NULL);

    FS_FreeFile(data);
    return ret;
}

/*
===============
IMG_LoadGlowMap

A glow map for the software renderer: <name>_glow, the rerelease's, beside
the image; for a wall also Q2RTX's <name>_light emissive map, beside it or in
overrides/. Its pixels as RGBA at its own size, which the caller frees with
Z_Free; it is not registered as an image. *from_light tells the two kinds
apart, since they are read differently. NULL when there is none.
===============
*/
byte *IMG_LoadGlowMap(const image_t *image, int *width, int *height, bool *from_light)
{
    char        base[MAX_IMAGE_PATH];
    const char  *slash;
    image_t     temp;
    byte        *pic;
    size_t      len;
    int         i, ret;

    if (COM_StripExtension(base, image->name, sizeof(base)) >= sizeof(base))
        return NULL;
    slash = strrchr(base, '/');

    for (i = 0; i < 3; i++) {
        if (i > 0 && image->type != IT_WALL)
            break;
        // a Quake 1 texture takes nothing from Quake II's overrides/ (IMG_Find)
        if (i == 2 && !Q_strncasecmp(base, Q1_TEXTURE_PREFIX, Q1_TEXTURE_PREFIX_LEN))
            break;

        memset(&temp, 0, sizeof(temp));
        if (i == 0)
            len = Q_concat(temp.name, sizeof(temp.name), base, "_glow.png");
        else if (i == 1)
            len = Q_concat(temp.name, sizeof(temp.name), base, "_light.png");
        else
            len = Q_concat(temp.name, sizeof(temp.name), "overrides/", slash ? slash + 1 : base, "_light.png");
        if (len >= sizeof(temp.name))
            continue;
        temp.baselen = len - 4;

        // any 32-bit format, then .pcx
        pic = NULL;
        ret = try_load_image_candidate(&temp, NULL, 0, &pic, IT_PIC, 0, true, TRY_IMAGE_SRC_BASE);
        if (ret >= 0 && pic && temp.upload_width > 0 && temp.upload_height > 0) {
            *width = temp.upload_width;
            *height = temp.upload_height;
            *from_light = i > 0;
            return pic;
        }
        if (pic)
            IMG_FreePixels(pic);
    }

    return NULL;
}

static const cmdreg_t img_cmd[] = {
    { "imagelist", IMG_List_f },
    { "screenshot", IMG_ScreenShot_f },
    { "screenshottga", IMG_ScreenShotTGA_f },
    { "screenshotjpg", IMG_ScreenShotJPG_f },
    { "screenshotpng", IMG_ScreenShotPNG_f },
    { "screenshothdr", IMG_ScreenShotHDR_f },
    { NULL }
};

void IMG_Init(void)
{
    int i;

    Q_assert(!r_numImages);

    r_override_textures = Cvar_Get("r_override_textures", "1", CVAR_FILES);
    r_texture_formats = Cvar_Get("r_texture_formats", "pjt", 0);
    r_texture_formats->changed = r_texture_formats_changed;
    r_texture_formats_changed(r_texture_formats);

    r_screenshot_format = Cvar_Get("gl_screenshot_format", "png", CVAR_ARCHIVE);
    r_screenshot_async = Cvar_Get("gl_screenshot_async", "1", 0);
    r_screenshot_quality = Cvar_Get("gl_screenshot_quality", "100", CVAR_ARCHIVE);
    r_screenshot_compression = Cvar_Get("gl_screenshot_compression", "6", CVAR_ARCHIVE);
    r_screenshot_message = Cvar_Get("gl_screenshot_message", "0", CVAR_ARCHIVE);
    r_screenshot_template = Cvar_Get("gl_screenshot_template", "quakeXXX", 0);

    Cmd_Register(img_cmd);

    for (i = 0; i < RIMAGES_HASH; i++) {
        List_Init(&r_imageHash[i]);
    }

    // &r_images[0] == R_NOTEXTURE
    r_numImages = 1;
}

void IMG_Shutdown(void)
{
    Cmd_Deregister(img_cmd);
    r_numImages = 0;
}
