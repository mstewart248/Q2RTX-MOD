// The perspective-correct span drawers, built once for each pixel format
// (scan.c), drawing with the surface's own gradients (spangrad_t), so any
// number of them can run at once. Define before including:
//   SPANS_FUNC, TURB_FUNC     names of the two drawers
//   TEXEL_BYTES, PIXEL_BYTES  bytes a texel and a pixel
//   PUT(pdest, ptex)          one texel to the screen

/*
=============
TURB_FUNC
=============
*/
void TURB_FUNC(espan_t *pspan, const spangrad_t *g, int *warptable)
{
    int             count, spancount;
    byte            *pbase, *pdest, *ptex;
    fixed16_t       s, t, snext, tnext, sstep, tstep;
    float           sdivz, tdivz, zi, z, du, dv, spancountminus1;
    float           sdivz16stepu, tdivz16stepu, zi16stepu;
    const float     d_sdivzstepu = g->sdivzstepu, d_tdivzstepu = g->tdivzstepu, d_zistepu = g->zistepu;
    const float     d_sdivzstepv = g->sdivzstepv, d_tdivzstepv = g->tdivzstepv, d_zistepv = g->zistepv;
    const float     d_sdivzorigin = g->sdivzorigin, d_tdivzorigin = g->tdivzorigin, d_ziorigin = g->ziorigin;
    const fixed16_t sadjust = g->sadjust, tadjust = g->tadjust;
    const fixed16_t bbextents = g->bbextents, bbextentt = g->bbextentt;
    int             *turb;
    int             turb_s, turb_t;

    turb = warptable + ((int)(r_newrefdef.time * SPEED) & (CYCLE - 1));

    sstep = 0;   // keep compiler happy
    tstep = 0;   // ditto

    pbase = g->cacheblock;

    sdivz16stepu = d_sdivzstepu * 16;
    tdivz16stepu = d_tdivzstepu * 16;
    zi16stepu = d_zistepu * 16;

    do {
        pdest = d_spantable[pspan->v] + pspan->u * PIXEL_BYTES;

        count = pspan->count;

        // calculate the initial s/z, t/z, 1/z, s, and t and clamp
        du = (float)pspan->u;
        dv = (float)pspan->v;

        sdivz = d_sdivzorigin + dv * d_sdivzstepv + du * d_sdivzstepu;
        tdivz = d_tdivzorigin + dv * d_tdivzstepv + du * d_tdivzstepu;
        zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
        z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point

        s = (int)(sdivz * z) + sadjust;
        if (s > bbextents)
            s = bbextents;
        else if (s < 0)
            s = 0;

        t = (int)(tdivz * z) + tadjust;
        if (t > bbextentt)
            t = bbextentt;
        else if (t < 0)
            t = 0;

        do {
            // calculate s and t at the far end of the span
            if (count >= 16)
                spancount = 16;
            else
                spancount = count;

            count -= spancount;

            if (q_likely(count)) {
                // calculate s/z, t/z, zi->fixed s and t at far end of span,
                // calculate s and t steps across span by shifting
                sdivz += sdivz16stepu;
                tdivz += tdivz16stepu;
                zi += zi16stepu;
                z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point

                snext = (int)(sdivz * z) + sadjust;
                if (snext > bbextents)
                    snext = bbextents;
                else if (snext < 16)
                    snext = 16; // prevent round-off error on <0 steps from
                                // from causing overstepping & running off the
                                // edge of the texture

                tnext = (int)(tdivz * z) + tadjust;
                if (tnext > bbextentt)
                    tnext = bbextentt;
                else if (tnext < 16)
                    tnext = 16; // guard against round-off error on <0 steps

                sstep = (snext - s) >> 4;
                tstep = (tnext - t) >> 4;
            } else {
                // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
                // can't step off polygon), clamp, calculate s and t steps across
                // span by division, biasing steps low so we don't run off the
                // texture
                spancountminus1 = (float)(spancount - 1);
                sdivz += d_sdivzstepu * spancountminus1;
                tdivz += d_tdivzstepu * spancountminus1;
                zi += d_zistepu * spancountminus1;
                z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point

                snext = (int)(sdivz * z) + sadjust;
                if (snext > bbextents)
                    snext = bbextents;
                else if (snext < 16)
                    snext = 16; // prevent round-off error on <0 steps from
                                // from causing overstepping & running off the
                                // edge of the texture

                tnext = (int)(tdivz * z) + tadjust;
                if (tnext > bbextentt)
                    tnext = bbextentt;
                else if (tnext < 16)
                    tnext = 16; // guard against round-off error on <0 steps

                if (spancount > 1) {
                    sstep = (snext - s) / (spancount - 1);
                    tstep = (tnext - t) / (spancount - 1);
                }
            }

            s = s & ((CYCLE << 16) - 1);
            t = t & ((CYCLE << 16) - 1);

            do {
                turb_s = ((s + turb[(t >> 16) & (CYCLE - 1)]) >> 16) & TURB_MASK;
                turb_t = ((t + turb[(s >> 16) & (CYCLE - 1)]) >> 16) & TURB_MASK;
                ptex = pbase + (turb_t * TURB_SIZE * TEXEL_BYTES) + turb_s * TEXEL_BYTES;
                PUT(pdest, ptex);
                pdest += PIXEL_BYTES;
                s += sstep;
                t += tstep;
            } while (--spancount > 0);

            s = snext;
            t = tnext;

        } while (count > 0);

    } while ((pspan = pspan->pnext) != NULL);
}

/*
=============
SPANS_FUNC
=============
*/
void SPANS_FUNC(espan_t *pspan, const spangrad_t *g)
{
    int             count, spancount;
    byte            *pbase, *pdest, *ptex;
    fixed16_t       s, t, snext, tnext, sstep, tstep;
    float           sdivz, tdivz, zi, z, du, dv, spancountminus1;
    float           sdivz16stepu, tdivz16stepu, zi16stepu;
    const float     d_sdivzstepu = g->sdivzstepu, d_tdivzstepu = g->tdivzstepu, d_zistepu = g->zistepu;
    const float     d_sdivzstepv = g->sdivzstepv, d_tdivzstepv = g->tdivzstepv, d_zistepv = g->zistepv;
    const float     d_sdivzorigin = g->sdivzorigin, d_tdivzorigin = g->tdivzorigin, d_ziorigin = g->ziorigin;
    const fixed16_t sadjust = g->sadjust, tadjust = g->tadjust;
    const fixed16_t bbextents = g->bbextents, bbextentt = g->bbextentt;
    const int       cachewidth = g->cachewidth;

    sstep = 0;  // keep compiler happy
    tstep = 0;  // ditto

    pbase = g->cacheblock;

    sdivz16stepu = d_sdivzstepu * 16;
    tdivz16stepu = d_tdivzstepu * 16;
    zi16stepu = d_zistepu * 16;

    do {
        pdest = d_spantable[pspan->v] + pspan->u * PIXEL_BYTES;

        count = pspan->count;

        // calculate the initial s/z, t/z, 1/z, s, and t and clamp
        du = (float)pspan->u;
        dv = (float)pspan->v;

        sdivz = d_sdivzorigin + dv * d_sdivzstepv + du * d_sdivzstepu;
        tdivz = d_tdivzorigin + dv * d_tdivzstepv + du * d_tdivzstepu;
        zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
        z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point

        s = (int)(sdivz * z) + sadjust;
        if (s > bbextents)
            s = bbextents;
        else if (s < 0)
            s = 0;

        t = (int)(tdivz * z) + tadjust;
        if (t > bbextentt)
            t = bbextentt;
        else if (t < 0)
            t = 0;

        do {
            // calculate s and t at the far end of the span
            if (count >= 16)
                spancount = 16;
            else
                spancount = count;

            count -= spancount;

            if (q_likely(count)) {
                // calculate s/z, t/z, zi->fixed s and t at far end of span,
                // calculate s and t steps across span by shifting
                sdivz += sdivz16stepu;
                tdivz += tdivz16stepu;
                zi += zi16stepu;
                z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point

                snext = (int)(sdivz * z) + sadjust;
                if (snext > bbextents)
                    snext = bbextents;
                else if (snext < 16)
                    snext = 16; // prevent round-off error on <0 steps from
                                // from causing overstepping & running off the
                                // edge of the texture

                tnext = (int)(tdivz * z) + tadjust;
                if (tnext > bbextentt)
                    tnext = bbextentt;
                else if (tnext < 16)
                    tnext = 16;  // guard against round-off error on <0 steps

                sstep = (snext - s) >> 4;
                tstep = (tnext - t) >> 4;
            } else {
                // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
                // can't step off polygon), clamp, calculate s and t steps across
                // span by division, biasing steps low so we don't run off the
                // texture
                spancountminus1 = (float)(spancount - 1);
                sdivz += d_sdivzstepu * spancountminus1;
                tdivz += d_tdivzstepu * spancountminus1;
                zi += d_zistepu * spancountminus1;
                z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point

                snext = (int)(sdivz * z) + sadjust;
                if (snext > bbextents)
                    snext = bbextents;
                else if (snext < 16)
                    snext = 16; // prevent round-off error on <0 steps from
                                // from causing overstepping & running off the
                                // edge of the texture

                tnext = (int)(tdivz * z) + tadjust;
                if (tnext > bbextentt)
                    tnext = bbextentt;
                else if (tnext < 16)
                    tnext = 16;  // guard against round-off error on <0 steps

                if (spancount > 1) {
                    sstep = (snext - s) / (spancount - 1);
                    tstep = (tnext - t) / (spancount - 1);
                }
            }

            do {
                ptex = pbase + (s >> 16) * TEXEL_BYTES + (t >> 16) * cachewidth;
                PUT(pdest, ptex);
                pdest += PIXEL_BYTES;
                s += sstep;
                t += tstep;
            } while (--spancount > 0);

            s = snext;
            t = tnext;

        } while (count > 0);

    } while ((pspan = pspan->pnext) != NULL);
}

#undef SPANS_FUNC
#undef TURB_FUNC
#undef TEXEL_BYTES
#undef PIXEL_BYTES
#undef PUT
