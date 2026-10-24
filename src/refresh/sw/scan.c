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
// d_scan.c
//
// Portable C scan-level rasterization code, all pixel depths.

#include "sw.h"

/*
=============
D_WarpScreen

this performs a slight compression of the screen at the same time as
the sine warp, to keep the edges from wrapping

The view was drawn off screen at r_refdef.vrect's size, which may be below
the window's (sw_scale); this resamples it into the refdef rectangle. The
wobble was 3 pixels in 128 at 240 lines, so it scales with the height to
keep that look at any resolution.
=============
*/
typedef struct {
    int         w, h, jobs;
    int         *turb;
    uint32_t    **rowptr;
    int         *column, *uphase, *vphase;
} warppass_t;

static void D_WarpRows(void *arg, int index, int thread)
{
    const warppass_t    *p = arg;
    const int           *turb = p->turb;
    int                 v0 = p->h * index / p->jobs;
    int                 v1 = p->h * (index + 1) / p->jobs;
    int                 u, v;
    uint32_t            *dest;
    int                 *col;
    uint32_t            **row;

    dest = (uint32_t *)(vid.buffer + (r_newrefdef.y + v0) * vid.rowbytes) + r_newrefdef.x;

    if (r_8bit) {
        for (v = v0; v < v1; v++, dest = (uint32_t *)((byte *)dest + vid.rowbytes)) {
            col = &p->column[turb[p->vphase[v]]];
            row = &p->rowptr[v];
            for (u = 0; u < p->w; u++) {
                dest[u] = r_palette[((byte *)row[turb[p->uphase[u]]])[col[u]]];
            }
        }
        return;
    }

    for (v = v0; v < v1; v++, dest = (uint32_t *)((byte *)dest + vid.rowbytes)) {
        col = &p->column[turb[p->vphase[v]]];
        row = &p->rowptr[v];
        for (u = 0; u < p->w; u++) {
            dest[u] = row[turb[p->uphase[u]]][col[u]];
        }
    }
}

void D_WarpScreen(void)
{
    int         w, h, amp, i;
    int         u, v, u2, v2;
    float       scale;
    warppass_t  p;

    static int      cached_width, cached_height, cached_vw, cached_vh, cached_rowbytes;
    static pixel_t  *cached_buffer;
    static uint32_t *rowptr[MAXHEIGHT * 2];
    static int      column[MAXWIDTH * 2];
    static int      uphase[MAXWIDTH], vphase[MAXHEIGHT];
    static int      warptable[CYCLE * 2];

    w = r_newrefdef.width;
    h = r_newrefdef.height;
    scale = h / 240.0f;
    amp = max(AMP2, (int)(AMP2 * scale));

    if (w != cached_width || h != cached_height || cached_buffer != d_viewbuffer ||
        cached_vw != r_refdef.vrect.width || cached_vh != r_refdef.vrect.height ||
        cached_rowbytes != d_screenrowbytes) {
        cached_rowbytes = d_screenrowbytes;
        cached_width = w;
        cached_height = h;
        cached_buffer = d_viewbuffer;
        cached_vw = r_refdef.vrect.width;
        cached_vh = r_refdef.vrect.height;

        for (v = 0; v < h + amp * 2; v++) {
            v2 = (int)((float)v / (h + amp * 2) * r_refdef.vrect.height);
            rowptr[v] = (uint32_t *)(d_viewbuffer + v2 * d_screenrowbytes);
        }

        for (u = 0; u < w + amp * 2; u++) {
            u2 = (int)((float)u / (w + amp * 2) * r_refdef.vrect.width);
            column[u] = u2;
        }

        for (v = 0; v < h; v++)
            vphase[v] = (int)(v / scale) & (CYCLE - 1);
        for (u = 0; u < w; u++)
            uphase[u] = (int)(u / scale) & (CYCLE - 1);

        for (i = 0; i < CYCLE * 2; i++)
            warptable[i] = amp + sin(i * M_PI * 2 / CYCLE) * amp;
    }

    p.w = w;
    p.h = h;
    p.turb = warptable + ((int)(r_newrefdef.time * SPEED) & (CYCLE - 1));
    p.rowptr = rowptr;
    p.column = column;
    p.uphase = uphase;
    p.vphase = vphase;
    p.jobs = R_RowJobs(h);
    R_ParallelFor(p.jobs, D_WarpRows, &p);
}

#define SPANS_FUNC      D_DrawSpans16
#define TURB_FUNC       D_DrawTurbulent16
#define TEXEL_BYTES     TEX_BYTES
#define PIXEL_BYTES     VID_BYTES
#define PUT(d, t)       ((d)[0] = (t)[2], (d)[1] = (t)[1], (d)[2] = (t)[0])
#include "span.h"

#define SPANS_FUNC      D_DrawSpans16_8
#define TURB_FUNC       D_DrawTurbulent16_8
#define TEXEL_BYTES     1
#define PIXEL_BYTES     1
#define PUT(d, t)       (*(d) = *(t))
#include "span.h"

/*
=============
D_DrawZSpans
=============
*/
void D_DrawZSpans(espan_t *pspan, float d_zistepu, float d_zistepv, float d_ziorigin)
{
    int             count, doublecount, izistep;
    int             izi;
    short           *pdest;
    uint32_t        ltemp;
    float           zi;
    float           du, dv;

// FIXME: check for clamping/range problems
// we count on FP exceptions being turned off to avoid range problems
    izistep = (int)(d_zistepu * 0x8000 * 0x10000);

    do {
        pdest = d_zspantable[pspan->v] + pspan->u;

        count = pspan->count;

        // calculate the initial 1/z
        du = (float)pspan->u;
        dv = (float)pspan->v;

        zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
        // we count on FP exceptions being turned off to avoid range problems
        izi = (int)(zi * 0x8000 * 0x10000);

        // the fog's depth, before the 16 bits drop most of it
        if (d_pfogz) {
            int32_t *pfz = d_pfogz + pspan->v * d_zwidth + pspan->u;
            int     i, fz = izi;

            for (i = 0; i < count; i++, fz += izistep)
                pfz[i] = fz;
        }

        if ((uintptr_t)pdest & 0x02) {
            *pdest++ = (short)(izi >> 16);
            izi += izistep;
            count--;
        }

        if ((doublecount = count >> 1) > 0) {
            do {
                ltemp = izi >> 16;
                izi += izistep;
                ltemp |= izi & 0xFFFF0000;
                izi += izistep;
                *(uint32_t *)pdest = ltemp;
                pdest += 2;
            } while (--doublecount > 0);
        }

        if (count & 1)
            *pdest = (short)(izi >> 16);

    } while ((pspan = pspan->pnext) != NULL);
}

