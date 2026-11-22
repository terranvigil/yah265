/*
 * Unit tests: --no-scenecut puts key frames on the --keyint grid only.
 * A synthetic five-shot clip, encoded through the public API: with the
 * flag every IRAP is on a multiple of keyint at two picture sizes, a
 * --qpfile K line and a live-control KEYFRAME still force an IDR, and
 * without it the cuts place key frames off the grid.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yah265.h"

#define MAXW 320
#define MAXH 192
#define NF 200
#define KEYINT 48

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, \
                                          __LINE__, #c); fails++; } } while (0)

static const int cuts[] = { 30, 75, 130, 170 };

static n265_pixel planes[3][MAXW * MAXH];
static int W, H;

/* each shot has its own texture, motion and level */
static void make_frame(int t)
{
    int shot = 0;
    for (int k = 0; k < (int)(sizeof cuts / sizeof cuts[0]); k++)
        if (t >= cuts[k])
            shot = k + 1;
    unsigned s = 977u * (unsigned)(shot + 1) + (unsigned)t * 7919u;
    int fx = 1 + shot * 3, fy = 2 + (shot * 5) % 7, base = 30 + shot * 37;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            s = s * 1103515245u + 12345u;
            /* coordinates in units of the 192-wide picture, so both
             * sizes show the same content */
            int xs = x * 192 / W, ys = y * 128 / H;
            int v = ((xs + (shot + 1) * t) * fx + (ys + t) * fy) % 90 + base +
                    (int)((s >> 16) & 7);
            planes[0][y * W + x] = (n265_pixel)(v > 235 ? 235 : v);
        }
    for (int c = 1; c < 3; c++)
        for (int i = 0; i < (W / 2) * (H / 2); i++)
            planes[c][i] = (n265_pixel)(128 + ((i + t * c + shot * 40) % 31) - 15);
}

static n265_config *cfg;

static n265_encoder *open_with(const char *opts, int w, int h)
{
    if (cfg)
        n265_config_free(cfg);
    cfg = n265_config_new();
    if (n265_config_parse_list(cfg, opts) != 0)
        return NULL;
    W = w;
    H = h;
    n265_param p;
    n265_param_default(&p);
    p.width = w;
    p.height = h;
    p.fps_num = 24;
    p.fps_den = 1;
    if (n265_config_apply(cfg, &p) != 0)
        return NULL;
    return n265_encoder_open(&p);
}

/* 0 not a key frame, 1 IDR, 2 CRA */
static int key[NF];

static void note(n265_encoder *e, const n265_nal *nal)
{
    const n265_au *au;
    int na = n265_encoder_access_units(e, &au);
    for (int i = 0; i < na; i++)
        for (int j = 0; j < au[i].nnal; j++) {
            int t = nal[au[i].first_nal + j].type, d = au[i].display_idx;
            if (d < 0 || d >= NF || t < 16 || t > 23)
                continue;
            key[d] = t == N265_NAL_CRA_NUT ? 2 : 1;
        }
}

/* encode the clip; ctl_at >= 0 sends a KEYFRAME for that frame */
static int encode(const char *opts, int w, int h, int ctl_at)
{
    memset(key, 0, sizeof key);
    n265_encoder *e = open_with(opts, w, h);
    CHECK(e);
    if (!e)
        return -1;
    if (ctl_at >= 0) {
        n265_control_t v;
        n265_control_init(&v, N265_CTL_KEYFRAME);
        v.frame = ctl_at;
        CHECK(n265_encoder_control(e, &v) == N265_CTL_OK);
    }
    n265_nal *nal;
    int nn;
    for (int t = 0; t < NF; t++) {
        make_frame(t);
        n265_picture p = { { planes[0], planes[1], planes[2] },
                           { W, W / 2, W / 2 }, t };
        CHECK(n265_encoder_encode(e, &nal, &nn, &p) >= 0);
        note(e, nal);
    }
    for (;;) {
        int r = n265_encoder_encode(e, &nal, &nn, NULL);
        note(e, nal);
        if (r <= 0 && nn == 0)
            break;
    }
    n265_encoder_close(e);
    return 0;
}

static int off_grid(void)
{
    int n = 0;
    for (int d = 0; d < NF; d++)
        n += key[d] && d % KEYINT;
    return n;
}

static int on_grid(int want_cra)
{
    for (int d = 0; d < NF; d += KEYINT)
        if (key[d] != (d && want_cra ? 2 : 1))
            return 0;
    return 1;
}

static void test_parse(void)
{
    double v = -1;
    n265_config *c = n265_config_new();
    CHECK(n265_config_get(c, "scenecut", &v) == 0 && v == 1);
    CHECK(n265_config_parse_list(c, "scenecut=0") == 0);
    CHECK(n265_config_get(c, "scenecut", &v) == 0 && v == 0);
    CHECK(n265_config_parse(c, "scenecut", NULL) == 0);
    CHECK(n265_config_get(c, "scenecut", &v) == 0 && v == 1);
    CHECK(n265_config_parse(c, "--no-scenecut", NULL) == 0);
    CHECK(n265_config_get(c, "scenecut", &v) == 0 && v == 0);
    n265_config_free(c);
}

int main(void)
{
    test_parse();
    /* the two ask for opposite things */
    CHECK(open_with("no-scenecut:shot-keys", 192, 128) == NULL);

    /* the clip's cuts are real: without the flag they place key frames */
    encode("qp=30:threads=1:keyint=48:no-open-gop", 192, 128, -1);
    CHECK(off_grid() >= 2);

    encode("qp=30:threads=1:keyint=48:no-open-gop:no-scenecut", 192, 128, -1);
    int small[NF];
    memcpy(small, key, sizeof key);
    CHECK(off_grid() == 0 && on_grid(0));

    /* a second size of the same source: the same frames */
    encode("qp=30:threads=1:keyint=48:no-open-gop:no-scenecut", 320, 192, -1);
    CHECK(!memcmp(small, key, sizeof key));

    /* open GOP: the same frames, CRA after the first */
    encode("qp=30:threads=1:keyint=48:no-scenecut", 192, 128, -1);
    CHECK(off_grid() == 0 && on_grid(1));

    /* auto threads: the placement is the lookahead's, not the schedule's */
    encode("qp=30:keyint=48:no-open-gop:no-scenecut", 192, 128, -1);
    CHECK(!memcmp(small, key, sizeof key));

    /* a live-control KEYFRAME still lands, and the grid holds after it */
    encode("qp=30:threads=1:keyint=48:no-open-gop:no-scenecut", 192, 128, 100);
    CHECK(key[100] == 1 && on_grid(0) && off_grid() == 1);

    /* a --qpfile K line still forces a key frame */
    char path[] = "/tmp/n265_scenecut_qpf_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "w");
        fputs("61 K\n", f);
        fclose(f);
        char opts[256];
        snprintf(opts, sizeof opts,
                 "qp=30:threads=1:keyint=48:no-open-gop:no-scenecut:"
                 "qpfile=%s", path);
        encode(opts, 192, 128, -1);
        CHECK(key[61] == 1 && on_grid(0) && off_grid() == 1);
        remove(path);
    }

    if (cfg)
        n265_config_free(cfg);
    if (fails)
        fprintf(stderr, "scenecut: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
