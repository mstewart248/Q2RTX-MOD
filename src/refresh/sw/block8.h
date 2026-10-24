// 8-bit surface cache block: palette texels lit through the colormap.
// blocklights here are colormap rows << 8 (R_BuildLightMap8).

#define BLOCK_SIZE  (1 << BLOCK_SHIFT)

static void BLOCK_FUNC(blockctx_t *c)
{
    int     lightleft, lightright, lightleftstep, lightrightstep;
    int     v, i, b, lightstep, light;
    byte    *psource, *prowdest;
    blocklight_t    *r_lightptr = c->lightptr;
    const int       r_lightwidth = c->lightwidth;
    const int       sourcetstep = c->sourcetstep;
    const int       surfrowbytes = c->surfrowbytes;

    psource = c->pbasesource;
    prowdest = c->prowdestbase;

    for (v = 0 ; v < c->numvblocks ; v++) {
        lightleft = r_lightptr[0];
        lightright = r_lightptr[1];
        r_lightptr += r_lightwidth;
        lightleftstep = (r_lightptr[0] - lightleft) >> BLOCK_SHIFT;
        lightrightstep = (r_lightptr[1] - lightright) >> BLOCK_SHIFT;

        for (i = 0 ; i < BLOCK_SIZE ; i++) {
            lightstep = (lightleft - lightright) >> BLOCK_SHIFT;
            light = lightright;

            for (b = BLOCK_SIZE - 1; b >= 0; b--) {
                prowdest[b] = r_colormap[(light & 0xFF00) + psource[b]];
                light += lightstep;
            }

            psource += sourcetstep;
            lightright += lightrightstep;
            lightleft += lightleftstep;
            prowdest += surfrowbytes;
        }

        if (psource >= c->sourcemax)
            psource -= c->stepback;
    }
}

#undef BLOCK_FUNC
#undef BLOCK_SIZE
#undef BLOCK_SHIFT
