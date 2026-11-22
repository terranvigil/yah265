/*
 * Unit tests: live control (src/encoder/ctl.c, the shared contract v1).
 * Every refusal code, the struct versioning, NEXT, ELATE, the flush's
 * applied_frame -1, the landing frames, and submission-time independence
 * at one thread, all through the public API on a small synthetic clip.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yah265.h"
#include "ctl_log.h"

void n265_set_hrd(int on);

#define W 192
#define H 128
#define NF 40

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, \
                                          __LINE__, #c); fails++; } } while (0)

static n265_pixel planes[3][W * H];

static void make_frame(int t)
{
    unsigned s = 12345u + (unsigned)t * 7919u;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            s = s * 1103515245u + 12345u;
            int v = ((x + 2 * t) * 3 + (y + t) * 2) % 200 + 20 +
                    (int)((s >> 16) & 7);
            planes[0][y * W + x] = (n265_pixel)v;
        }
    for (int c = 1; c < 3; c++)
        for (int i = 0; i < (W / 2) * (H / 2); i++)
            planes[c][i] = (n265_pixel)(128 + ((i + t * c) % 31) - 15);
}

typedef struct {
    unsigned char *buf;
    size_t len, cap;
} sink;

static void put(sink *k, const n265_nal *nal, int n)
{
    for (int i = 0; i < n; i++) {
        if (k->len + (size_t)nal[i].size > k->cap) {
            k->cap = 2 * (k->len + (size_t)nal[i].size);
            k->buf = realloc(k->buf, k->cap);
        }
        memcpy(k->buf + k->len, nal[i].payload, (size_t)nal[i].size);
        k->len += (size_t)nal[i].size;
    }
}

static n265_config *cfg;

static n265_encoder *open_with(const char *opts)
{
    if (cfg)
        n265_config_free(cfg);
    cfg = n265_config_new();
    if (n265_config_parse_list(cfg, opts) != 0)
        return NULL;
    n265_param p;
    n265_param_default(&p);
    p.width = W;
    p.height = H;
    p.fps_num = 30;
    p.fps_den = 1;
    if (n265_config_apply(cfg, &p) != 0)
        return NULL;
    return n265_encoder_open(&p);
}

static n265_picture pic(void)
{
    n265_picture p = { { planes[0], planes[1], planes[2] },
                       { W, W / 2, W / 2 }, 0 };
    return p;
}

static int idr_seen[NF];

static void note_idr(n265_encoder *e, const n265_nal *nal)
{
    const n265_au *au;
    int na = n265_encoder_access_units(e, &au);
    for (int i = 0; i < na; i++)
        for (int j = 0; j < au[i].nnal; j++)
            if (nal[au[i].first_nal + j].type == N265_NAL_IDR_W_RADL &&
                au[i].display_idx >= 0 && au[i].display_idx < NF)
                idr_seen[au[i].display_idx] = 1;
}

static int feed(n265_encoder *e, int t, sink *k)
{
    n265_nal *nal;
    int nn;
    make_frame(t);
    n265_picture p = pic();
    p.pts = t;
    int r = n265_encoder_encode(e, &nal, &nn, &p);
    note_idr(e, nal);
    if (k)
        put(k, nal, nn);
    return r;
}

static void flush(n265_encoder *e, sink *k)
{
    n265_nal *nal;
    int nn;
    for (;;) {
        int r = n265_encoder_encode(e, &nal, &nn, NULL);
        note_idr(e, nal);
        if (k)
            put(k, nal, nn);
        if (r <= 0 && nn == 0)
            break;
    }
}

static int ctl(n265_encoder *e, int kind, int64_t frame, n265_control_t *out)
{
    n265_control_t v;
    n265_control_init(&v, kind);
    v.frame = frame;
    if (out)
        v = *out, v.kind = kind, v.frame = frame;
    int r = n265_encoder_control(e, &v);
    if (out)
        *out = v;
    return r;
}

static void test_basics(void)
{
    n265_control_t v;
    n265_control_init(&v, N265_CTL_KEYFRAME);
    CHECK(v.size == (int)sizeof v && v.frame == N265_CTL_NEXT &&
          v.applied_frame == -1 && v.qp_min == -1 && v.qp_max == -1 &&
          v.last == -1);
    for (int c = 0; c >= -8; c--)
        CHECK(n265_control_strerror(c) && *n265_control_strerror(c));
    CHECK(n265_encoder_control(NULL, &v) == N265_CTL_EINVAL);
    CHECK(n265_encoder_controls_applied(NULL, &v, 1) == -1);
}

/* CQP: QP_LIMITS is EMODE, KEYFRAME and ZONE land exactly */
static void test_cqp(void)
{
    memset(idr_seen, 0, sizeof idr_seen);
    n265_encoder *e = open_with("qp=30:threads=1:keyint=250");
    CHECK(e);
    n265_control_t v;
    n265_control_init(&v, N265_CTL_KEYFRAME);
    v.size = 8;
    CHECK(n265_encoder_control(e, &v) == N265_CTL_EINVAL);
    v.size = (int)sizeof v + 8;
    CHECK(n265_encoder_control(e, &v) == N265_CTL_EINVAL);
    /* a v1 caller's exact size works */
    n265_control_init(&v, N265_CTL_KEYFRAME);
    v.size = (int)(offsetof(n265_control_t, height) + sizeof(int));
    v.frame = 7;
    CHECK(n265_encoder_control(e, &v) == N265_CTL_OK);
    CHECK(ctl(e, 0, 0, NULL) == N265_CTL_EINVAL);
    CHECK(ctl(e, N265_CTL_FPS, 0, NULL) == N265_CTL_EUNSUPPORTED);
    CHECK(ctl(e, N265_CTL_PRESET, 0, NULL) == N265_CTL_EUNSUPPORTED);
    CHECK(ctl(e, N265_CTL_RESOLUTION, 0, NULL) == N265_CTL_EUNSUPPORTED);
    CHECK(ctl(e, 99, 0, NULL) == N265_CTL_EUNSUPPORTED);
    CHECK(ctl(e, N265_CTL_KEYFRAME, -5, NULL) == N265_CTL_EINVAL);

    n265_control_init(&v, 0);
    v.qp_min = 20;
    CHECK(ctl(e, N265_CTL_QP_LIMITS, 3, &v) == N265_CTL_EMODE);
    n265_control_init(&v, 0);
    v.crf = 25;
    CHECK(ctl(e, N265_CTL_CRF, 3, &v) == N265_CTL_EMODE);
    n265_control_init(&v, 0);
    v.bitrate_kbps = 300;
    CHECK(ctl(e, N265_CTL_RATE, 3, &v) == N265_CTL_EMODE);
    n265_control_init(&v, 0);
    CHECK(ctl(e, N265_CTL_RATE, 3, &v) == N265_CTL_EINVAL);

    /* zones: last before frame, overlap */
    n265_control_init(&v, 0);
    v.last = 4;
    v.qp_offset = 2;
    CHECK(ctl(e, N265_CTL_ZONE, 5, &v) == N265_CTL_EINVAL);
    n265_control_init(&v, 0);
    v.last = 20;
    v.qp_offset = -3;
    CHECK(ctl(e, N265_CTL_ZONE, 10, &v) == N265_CTL_OK);
    n265_control_init(&v, 0);
    v.last = 12;
    v.qp_offset = 1;
    CHECK(ctl(e, N265_CTL_ZONE, 5, &v) == N265_CTL_ECONFLICT);
    v.qp_offset = 99;
    CHECK(ctl(e, N265_CTL_ZONE, 30, &v) == N265_CTL_EINVAL);

    /* NEXT resolves to frames_in; ELATE once handed in */
    for (int t = 0; t < 3; t++)
        CHECK(feed(e, t, NULL) >= 0);
    n265_control_init(&v, N265_CTL_KEYFRAME);
    CHECK(n265_encoder_control(e, &v) == N265_CTL_OK && v.frame == 3);
    CHECK(ctl(e, N265_CTL_KEYFRAME, 2, NULL) == N265_CTL_ELATE);
    CHECK(ctl(e, N265_CTL_KEYFRAME, 13, NULL) == N265_CTL_OK);
    CHECK(ctl(e, N265_CTL_KEYFRAME, 1000, NULL) == N265_CTL_OK);
    n265_status_t st;
    memset(&st, 0, sizeof st);
    st.size = 4;
    CHECK(n265_encoder_status(e, &st) == -1);
    st.size = (int)sizeof st;
    CHECK(n265_encoder_status(e, &st) == 0);
    CHECK(st.frames_in == 3 && st.next_frame == 3 &&
          st.controls_pending == 5 && st.target_kbps == 0.0);
    for (int t = 3; t < NF; t++)
        CHECK(feed(e, t, NULL) >= 0);
    flush(e, NULL);
    CHECK(idr_seen[0] && idr_seen[3] && idr_seen[7] && idr_seen[13]);
    n265_control_t out[16];
    for (int i = 0; i < 16; i++)
        n265_control_init(&out[i], 0);
    int n = n265_encoder_controls_applied(e, out, 16);
    CHECK(n == 5);
    int late = 0, landed = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].frame == 1000)
            late += out[i].applied_frame == -1;
        else
            landed += out[i].applied_frame == out[i].frame;
    }
    CHECK(late == 1 && landed == 4);
    CHECK(n265_encoder_status(e, &st) == 0 && st.controls_pending == 0 &&
          st.controls_applied == 4 && st.frames_out == NF &&
          st.queue_depth == 0);
    n265_encoder_close(e);
}

/* CRF: the CRF range, RATE needs a buffer, open-time zones conflict */
static void test_crf(void)
{
    n265_encoder *e = open_with("crf=28:threads=1:zones=10,19,b=1.5");
    CHECK(e);
    n265_control_t v;
    n265_control_init(&v, 0);
    v.crf = 60;
    CHECK(ctl(e, N265_CTL_CRF, 0, &v) == N265_CTL_EINVAL);
    v.crf = 24.5;
    CHECK(ctl(e, N265_CTL_CRF, 0, &v) == N265_CTL_OK);
    n265_control_init(&v, 0);
    v.vbv_maxrate_kbps = 500;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_EMODE);
    n265_control_init(&v, 0);
    v.last = 12;
    v.qp_offset = 1;
    CHECK(ctl(e, N265_CTL_ZONE, 5, &v) == N265_CTL_ECONFLICT);
    n265_control_init(&v, 0);
    v.qp_min = 40;
    v.qp_max = 30;
    CHECK(ctl(e, N265_CTL_QP_LIMITS, 0, &v) == N265_CTL_EINVAL);
    v.qp_min = 52;
    v.qp_max = -1;
    CHECK(ctl(e, N265_CTL_QP_LIMITS, 0, &v) == N265_CTL_EINVAL);
    v.qp_min = 30;
    CHECK(ctl(e, N265_CTL_QP_LIMITS, 4, &v) == N265_CTL_OK);
    n265_control_init(&v, 0);
    v.qp_max = 25;      /* below the queued floor */
    CHECK(ctl(e, N265_CTL_QP_LIMITS, 6, &v) == N265_CTL_EINVAL);
    n265_encoder_close(e);

    /* capped CRF takes maxrate and bufsize, never a bitrate */
    e = open_with("crf=28:threads=1:vbv-maxrate=800:vbv-bufsize=800");
    CHECK(e);
    n265_control_init(&v, 0);
    v.bitrate_kbps = 400;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_EMODE);
    n265_control_init(&v, 0);
    v.vbv_maxrate_kbps = 600;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_OK);
    v.vbv_maxrate_kbps = 900;   /* above the signalled HRD */
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_EHRD);
    n265_control_init(&v, 0);
    v.qp_max = 40;
    CHECK(ctl(e, N265_CTL_QP_LIMITS, 0, &v) == N265_CTL_EHRD);
    n265_encoder_close(e);
}

static void test_abr(void)
{
    n265_control_t v;
    /* CBR stays CBR, and a bitrate alone carries the maxrate with it */
    n265_encoder *e = open_with("bitrate=500:vbv-maxrate=500:"
                                "vbv-bufsize=500:threads=1:no-hrd");
    CHECK(e);
    n265_control_init(&v, 0);
    v.bitrate_kbps = 300;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_OK &&
          v.vbv_maxrate_kbps == 300);
    n265_control_init(&v, 0);
    v.vbv_maxrate_kbps = 600;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_EMODE);
    n265_control_init(&v, 0);
    v.crf = 25;
    CHECK(ctl(e, N265_CTL_CRF, 0, &v) == N265_CTL_EMODE);
    n265_encoder_close(e);

    /* capped VBR keeps maxrate above the bitrate */
    e = open_with("bitrate=500:vbv-maxrate=750:vbv-bufsize=750:threads=1");
    CHECK(e);
    n265_control_init(&v, 0);
    v.vbv_maxrate_kbps = 400;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_EMODE);
    /* VBR HRD: decreases only */
    n265_control_init(&v, 0);
    v.bitrate_kbps = 300;
    v.vbv_maxrate_kbps = 450;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_OK);
    n265_control_init(&v, 0);
    v.vbv_bufsize_kbit = 1000;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_EHRD);
    n265_encoder_close(e);

    /* a CBR declaration refuses every RATE */
    e = open_with("bitrate=500:vbv-maxrate=500:vbv-bufsize=500:"
                  "strict-cbr:threads=1");
    CHECK(e);
    n265_control_init(&v, 0);
    v.bitrate_kbps = 300;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_EHRD);
    n265_encoder_close(e);

    /* the declared level bounds the rate */
    e = open_with("bitrate=500:vbv-maxrate=750:vbv-bufsize=750:threads=1:"
                  "level=2:no-hrd");
    CHECK(e);
    n265_control_init(&v, 0);
    v.bitrate_kbps = 4000;
    v.vbv_maxrate_kbps = 5000;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_ELEVEL);
    n265_control_init(&v, 0);
    v.bitrate_kbps = 800;
    v.vbv_maxrate_kbps = 1000;
    CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_OK);
    n265_encoder_close(e);

    /* two-pass refuses RATE, CRF and KEYFRAME */
    char stats[64];
    snprintf(stats, sizeof stats, "pass=1:stats=ctl_test_%d.log", (int)NF);
    char opts[160];
    snprintf(opts, sizeof opts, "bitrate=500:threads=1:%s", stats);
    e = open_with(opts);
    CHECK(e);
    if (e) {
        CHECK(ctl(e, N265_CTL_KEYFRAME, 0, NULL) == N265_CTL_EMODE);
        n265_control_init(&v, 0);
        v.bitrate_kbps = 300;
        CHECK(ctl(e, N265_CTL_RATE, 0, &v) == N265_CTL_EMODE);
        n265_encoder_close(e);
    }
    remove("ctl_test_40.log");
}

/* One event list, submitted all at once or each just before its landing
 * frame: the same bytes at one thread. */
static void run_list(const char *opts, const n265_control_t *ev, int nev,
                     int upfront, sink *k)
{
    n265_encoder *e = open_with(opts);
    CHECK(e);
    if (!e)
        return;
    int next = 0;
    for (int t = 0; t < NF; t++) {
        while (next < nev && (upfront || ev[next].frame == t)) {
            n265_control_t v = ev[next++];
            int r = n265_encoder_control(e, &v);
            if (r)
                fprintf(stderr, "kind %d frame %lld: %s\n", v.kind,
                        (long long)v.frame, n265_control_strerror(r));
            CHECK(r == N265_CTL_OK);
        }
        CHECK(feed(e, t, k) >= 0);
    }
    flush(e, k);
    n265_encoder_close(e);
}

static void independence(const char *opts, const n265_control_t *ev, int nev)
{
    sink a = { 0 }, b = { 0 }, z = { 0 };
    run_list(opts, ev, nev, 1, &a);
    run_list(opts, ev, nev, 0, &b);
    run_list(opts, ev, 0, 1, &z);
    CHECK(a.len && a.len == b.len && !memcmp(a.buf, b.buf, a.len));
    CHECK(!(a.len == z.len && !memcmp(a.buf, z.buf, a.len)));
    free(a.buf);
    free(b.buf);
    free(z.buf);
}

/* the library's configuration is process-global and a bitrate, once
 * set, wins over the crf: the CRF cases run before the ABR ones */
static void test_independence_crf(void)
{
    n265_control_t ev[6];
    for (int i = 0; i < 6; i++)
        n265_control_init(&ev[i], 0);
    ev[0].kind = N265_CTL_KEYFRAME; ev[0].frame = 9;
    ev[1].kind = N265_CTL_QP_LIMITS; ev[1].frame = 12; ev[1].qp_min = 33;
    ev[2].kind = N265_CTL_ZONE; ev[2].frame = 15; ev[2].last = 22;
    ev[2].qp_offset = -2.5;
    ev[3].kind = N265_CTL_CRF; ev[3].frame = 20; ev[3].crf = 22;
    ev[4].kind = N265_CTL_QP_LIMITS; ev[4].frame = 25; ev[4].qp_min = 0;
    independence("crf=28:threads=1", ev, 5);
}

static void test_independence_rate(void)
{
    n265_control_t ev[6];

    for (int i = 0; i < 6; i++)
        n265_control_init(&ev[i], 0);
    ev[0].kind = N265_CTL_RATE; ev[0].frame = 10; ev[0].bitrate_kbps = 200;
    ev[0].vbv_maxrate_kbps = 300; ev[0].vbv_bufsize_kbit = 300;
    ev[1].kind = N265_CTL_KEYFRAME; ev[1].frame = 17;
    ev[2].kind = N265_CTL_RATE; ev[2].frame = 25; ev[2].bitrate_kbps = 600;
    ev[2].vbv_maxrate_kbps = 900;
    independence("bitrate=400:vbv-maxrate=600:vbv-bufsize=600:threads=1:"
                 "no-hrd", ev, 3);
}

/* the shared grammar, as the CLI uses it */
static void test_log(void)
{
    struct ctl_log_event ev;
    char err[128], line[256];
    CHECK(ctl_log_header("#livectl 1") == 1);
    CHECK(ctl_log_header("# a comment") == 0);
    CHECK(ctl_log_header("#livectl x") == -1);
    CHECK(ctl_log_parse("  # note", 0, &ev, err, sizeof err) == 0);
    CHECK(ctl_log_parse("", 0, &ev, err, sizeof err) == 0);
    CHECK(ctl_log_parse("next key", 0, &ev, err, sizeof err) == -1);
    CHECK(ctl_log_parse("next key", 1, &ev, err, sizeof err) == 1 &&
          ev.frame == CTL_FRAME_NEXT && ev.kind == N265_CTL_KEYFRAME);
    CHECK(ctl_log_parse("5 jump", 0, &ev, err, sizeof err) == -1);
    CHECK(ctl_log_parse("5 key type=open", 0, &ev, err, sizeof err) == -1);
    CHECK(ctl_log_parse("5 rate", 0, &ev, err, sizeof err) == -1);
    CHECK(ctl_log_parse("5 rate bitrate=1 bitrate=2", 0, &ev, err,
                        sizeof err) == -1);
    CHECK(ctl_log_parse("5 qp min=", 0, &ev, err, sizeof err) == -1);
    CHECK(ctl_log_parse("5 zone last=9", 0, &ev, err, sizeof err) == -1);
    CHECK(ctl_log_parse("120\trate bitrate=2000  maxrate=2000\r\n", 0, &ev,
                        err, sizeof err) == 1 && ev.frame == 120 &&
          ev.kind == N265_CTL_RATE && ev.bitrate_kbps == 2000 &&
          ev.vbv_maxrate_kbps == 2000 && ev.vbv_bufsize_kbit == 0);
    ctl_log_format(&ev, line, sizeof line);
    CHECK(!strcmp(line, "120 rate bitrate=2000 maxrate=2000"));
    CHECK(ctl_log_parse("400 zone last=459 offset=-2.5", 0, &ev, err,
                        sizeof err) == 1);
    ctl_log_format(&ev, line, sizeof line);
    CHECK(!strcmp(line, "400 zone last=459 offset=-2.5"));
}

int main(void)
{
    test_log();
    test_basics();
    test_cqp();
    test_crf();
    test_independence_crf();
    test_abr();
    test_independence_rate();
    if (cfg)
        n265_config_free(cfg);
    if (fails)
        fprintf(stderr, "ctl_test: %d failures\n", fails);
    else
        printf("ctl_test: OK\n");
    return fails != 0;
}
