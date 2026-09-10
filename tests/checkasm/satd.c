/*
 * checkasm/satd.c - the SATD/SAD family (class N265_ASM_SATD).
 * Split out of checkasm.c unchanged on 2026-09-10; see checkasm.h for the
 * harness and the convention.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "encoder/satd.h"
#include "encoder/arm/neon.h"   /* self-guarded; the family kernels land here */

/* ---- SATD ---------------------------------------------------------- */

struct sctx {
    int w, h;
    uint8_t a[64 * 64], b[64 * 64];
};

/* the tiles the encoder actually asks for: squares 4..32, the
 * rectangular PUs, and the AMP shapes whose 12 and 4 sides fall to the
 * 4x4 tiling */
static const int satd_shapes[][2] = {
    { 4, 4 }, { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
    { 8, 4 }, { 4, 8 }, { 16, 8 }, { 8, 16 }, { 32, 16 }, { 16, 32 },
    { 16, 4 }, { 4, 16 }, { 12, 16 }, { 16, 12 }, { 24, 32 }, { 32, 24 },
};

static void satd_ref_call(void *p)
{
    struct sctx *c = p;
    uint32_t sum = 0;
    if ((c->w & 7) || (c->h & 7)) {
        for (int y = 0; y < c->h; y += 4)
            for (int x = 0; x < c->w; x += 4)
                sum += n265_satd4_ref(c->a + y * 64 + x, 64,
                                      c->b + y * 64 + x, 64);
    } else {
        for (int y = 0; y < c->h; y += 8)
            for (int x = 0; x < c->w; x += 8)
                sum += n265_satd8_ref(c->a + y * 64 + x, 64,
                                      c->b + y * 64 + x, 64);
    }
    c->a[4095] = (uint8_t)sum;      /* keep the call live */
}

static void satd_opt_call(void *p)
{
    struct sctx *c = p;
    c->a[4095] = (uint8_t)n265_satd_wh(c->a, 64, c->b, 64, c->w, c->h);
}

static uint32_t satd_ref_wh(const uint8_t *a, int as, const uint8_t *b,
                            int bs, int w, int h)
{
    uint32_t sum = 0;
    if ((w & 7) || (h & 7)) {
        for (int y = 0; y < h; y += 4)
            for (int x = 0; x < w; x += 4)
                sum += n265_satd4_ref(a + y * as + x, as, b + y * bs + x, bs);
        return sum;
    }
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8)
            sum += n265_satd8_ref(a + y * as + x, as, b + y * bs + x, bs);
    return sum;
}

int check_satd(void)
{
    static struct sctx c;
    int bad = 0;
    int nsh = (int)(sizeof(satd_shapes) / sizeof(satd_shapes[0]));

    for (int s = 0; s < nsh; s++) {
        c.w = satd_shapes[s][0];
        c.h = satd_shapes[s][1];
        for (int kind = 0; kind < 3; kind++) {
            for (int rep = 0; rep < 128; rep++) {
                for (int i = 0; i < 64 * 64; i++) {
                    switch (kind) {
                    case 0:
                        c.a[i] = (uint8_t)(rnd() >> 24);
                        c.b[i] = (uint8_t)(rnd() >> 24);
                        break;
                    case 1:     /* the extremes the abs-sum has to survive */
                        c.a[i] = (uint8_t)((rnd() & 1) ? 255 : 0);
                        c.b[i] = (uint8_t)((rnd() & 1) ? 255 : 0);
                        break;
                    default:    /* near-identical: the common encoder case */
                        c.a[i] = (uint8_t)(rnd() >> 24);
                        c.b[i] = c.a[i];
                        break;
                    }
                }
                uint32_t r = satd_ref_wh(c.a, 64, c.b, 64, c.w, c.h);
                uint32_t o = n265_satd_wh(c.a, 64, c.b, 64, c.w, c.h);
                if (r != o) {
                    printf("    satd MISMATCH %dx%d kind %d: ref %u opt %u\n",
                           c.w, c.h, kind, r, o);
                    bad++;
                    rep = 128;
                }
            }
        }
    }
    if (bad)
        return bad;

    for (int s = 0; s < nsh; s++) {
        char name[32];
        c.w = satd_shapes[s][0];
        c.h = satd_shapes[s][1];
        for (int i = 0; i < 64 * 64; i++) {
            c.a[i] = (uint8_t)(rnd() >> 24);
            c.b[i] = (uint8_t)(rnd() >> 24);
        }
        snprintf(name, sizeof(name), "satd %dx%d", c.w, c.h);
        bench(name, satd_ref_call, satd_opt_call, NULL, &c, 40000);
    }
    return 0;
}

