#define BLOCK_SIZE  (1 << BLOCK_SHIFT)

static void BLOCK_FUNC(blockctx_t *c)
{
    int     lightleft[3], lightright[3];
    int     lightleftstep[3], lightrightstep[3];
    int     v, i, b, lightstep[3], light[3];
    byte    *psource, *prowdest;
    blocklight_t    *r_lightptr = c->lightptr;
    const int       r_lightwidth = c->lightwidth;
    const int       sourcetstep = c->sourcetstep;
    const int       surfrowbytes = c->surfrowbytes;
#ifdef BLOCK_GLOW
    byte    *pglow = c->pbaseglow;  // the glow map, laid out as the texture
#endif

    psource = c->pbasesource;
    prowdest = c->prowdestbase;

    for (v = 0 ; v < c->numvblocks ; v++) {
        // FIXME: use delta rather than both right and left, like ASM?
        lightleft[0] = r_lightptr[0 * LIGHTMAP_BYTES + 0];
        lightleft[1] = r_lightptr[0 * LIGHTMAP_BYTES + 1];
        lightleft[2] = r_lightptr[0 * LIGHTMAP_BYTES + 2];
        lightright[0] = r_lightptr[1 * LIGHTMAP_BYTES + 0];
        lightright[1] = r_lightptr[1 * LIGHTMAP_BYTES + 1];
        lightright[2] = r_lightptr[1 * LIGHTMAP_BYTES + 2];
        r_lightptr += r_lightwidth;
        lightleftstep[0] = (r_lightptr[0 * LIGHTMAP_BYTES + 0] - lightleft[0]) >> BLOCK_SHIFT;
        lightleftstep[1] = (r_lightptr[0 * LIGHTMAP_BYTES + 1] - lightleft[1]) >> BLOCK_SHIFT;
        lightleftstep[2] = (r_lightptr[0 * LIGHTMAP_BYTES + 2] - lightleft[2]) >> BLOCK_SHIFT;
        lightrightstep[0] = (r_lightptr[1 * LIGHTMAP_BYTES + 0] - lightright[0]) >> BLOCK_SHIFT;
        lightrightstep[1] = (r_lightptr[1 * LIGHTMAP_BYTES + 1] - lightright[1]) >> BLOCK_SHIFT;
        lightrightstep[2] = (r_lightptr[1 * LIGHTMAP_BYTES + 2] - lightright[2]) >> BLOCK_SHIFT;

        for (i = 0 ; i < BLOCK_SIZE ; i++) {
            lightstep[0] = (lightleft[0] - lightright[0]) >> BLOCK_SHIFT;
            lightstep[1] = (lightleft[1] - lightright[1]) >> BLOCK_SHIFT;
            lightstep[2] = (lightleft[2] - lightright[2]) >> BLOCK_SHIFT;

            light[0] = lightright[0];
            light[1] = lightright[1];
            light[2] = lightright[2];

            for (b = BLOCK_SIZE - 1; b >= 0; b--) {
#ifdef BLOCK_GLOW
                // the glow takes the light toward a full lightmap, as far as
                // its alpha says: KEX's mix(lightmap, 1, glow.a) before its
                // gl_modulate 2, which is the 2x of this scale
                int g = pglow[b] + (pglow[b] >> 7);
                int l0 = light[0] + (((0xFFFF - light[0]) * g) >> 8);
                int l1 = light[1] + (((0xFFFF - light[1]) * g) >> 8);
                int l2 = light[2] + (((0xFFFF - light[2]) * g) >> 8);

                prowdest[b * TEX_BYTES + 0] = min((psource[b * TEX_BYTES + 0] * l0) >> 15, 255);
                prowdest[b * TEX_BYTES + 1] = min((psource[b * TEX_BYTES + 1] * l1) >> 15, 255);
                prowdest[b * TEX_BYTES + 2] = min((psource[b * TEX_BYTES + 2] * l2) >> 15, 255);
#else
                // light is 8.8 lightmap units: 128 is the texture as it is
                // and 255 twice that, as the colormap's top rows were
                prowdest[b * TEX_BYTES + 0] = min((psource[b * TEX_BYTES + 0] * light[0]) >> 15, 255);
                prowdest[b * TEX_BYTES + 1] = min((psource[b * TEX_BYTES + 1] * light[1]) >> 15, 255);
                prowdest[b * TEX_BYTES + 2] = min((psource[b * TEX_BYTES + 2] * light[2]) >> 15, 255);
#endif
                light[0] += lightstep[0];
                light[1] += lightstep[1];
                light[2] += lightstep[2];
            }

            psource += sourcetstep;
#ifdef BLOCK_GLOW
            pglow += sourcetstep / TEX_BYTES;
#endif
            lightright[0] += lightrightstep[0];
            lightright[1] += lightrightstep[1];
            lightright[2] += lightrightstep[2];
            lightleft[0] += lightleftstep[0];
            lightleft[1] += lightleftstep[1];
            lightleft[2] += lightleftstep[2];
            prowdest += surfrowbytes;
        }

        if (psource >= c->sourcemax) {
            psource -= c->stepback;
#ifdef BLOCK_GLOW
            pglow -= c->stepback / TEX_BYTES;
#endif
        }
    }
}

#undef BLOCK_GLOW
#undef BLOCK_FUNC
#undef BLOCK_SIZE
#undef BLOCK_SHIFT
