/*
 * The playground's entry points: the library's config, encode and recon
 * dump behind a handful of calls a Web Worker can make. One encoder at a
 * time. The stream goes to /out.hevc and the reconstruction, in display
 * order, to /recon.yuv, both on the in-memory file system. /aus.bin gets
 * one pair of int32 per access unit in coding order, its display index
 * and its byte count, which is what the page's MP4 writer needs.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>

#include <emscripten/emscripten.h>

#include "yah265.h"

void n265_encoder_set_dump(n265_encoder *e, FILE *f);

static n265_config *cfg;
static n265_encoder *enc;
static FILE *out, *recon, *aus;
static unsigned char *frame;
static int fw, fh;
static long long nbytes;
static int reorder;

static int write_nals(int r, n265_nal *nal, int nnal)
{
    for (int i = 0; r > 0 && i < nnal; i++)
        if (fwrite(nal[i].payload, 1, (size_t)nal[i].size, out) !=
            (size_t)nal[i].size)
            return -1;
    if (r > 0) {
        const n265_au *au;
        int n = n265_encoder_access_units(enc, &au);
        for (int k = 0; k < n; k++) {
            int rec[2] = { au[k].display_idx, 0 };
            for (int j = au[k].first_nal; j < au[k].first_nal + au[k].nnal; j++)
                rec[1] += nal[j].size;
            if (fwrite(rec, sizeof rec, 1, aus) != 1)
                return -1;
        }
    }
    if (r > 0)
        nbytes += r;
    return r;
}

static void pg_free(void)
{
    if (enc)
        n265_encoder_close(enc);
    if (cfg)
        n265_config_free(cfg);
    if (out)
        fclose(out);
    if (recon)
        fclose(recon);
    if (aus)
        fclose(aus);
    free(frame);
    enc = NULL;
    cfg = NULL;
    out = recon = aus = NULL;
    frame = NULL;
}

/* opts is x265-params style, "preset=fast:crf=28"; 0 or -1 */
EMSCRIPTEN_KEEPALIVE int pg_open(int width, int height, int fps_num,
                                 int fps_den, const char *opts)
{
    pg_free();
    cfg = n265_config_new();
    if (!cfg || n265_config_parse_list(cfg, opts) < 0 ||
        n265_config_parse(cfg, "threads", "1") < 0)
        return -1;
    n265_param p;
    n265_param_default(&p);
    p.width = width;
    p.height = height;
    p.csp = N265_CSP_I420;
    p.fps_num = fps_num;
    p.fps_den = fps_den;
    if (n265_config_apply(cfg, &p) < 0)
        return -1;
    enc = n265_encoder_open(&p);
    out = fopen("/out.hevc", "wb");
    recon = fopen("/recon.yuv", "w+b");
    aus = fopen("/aus.bin", "wb");
    frame = malloc((size_t)width * height * 3 / 2);
    if (!enc || !out || !recon || !aus || !frame) {
        pg_free();
        return -1;
    }
    n265_encoder_set_dump(enc, recon);
    fw = width;
    fh = height;
    nbytes = 0;
    reorder = n265_encoder_reorder_depth(enc);
    return 0;
}

/* sps_max_num_reorder_pics, for the MP4's composition offsets */
EMSCRIPTEN_KEEPALIVE int pg_reorder(void)
{
    return reorder;
}

/* where the caller writes the next I420 picture */
EMSCRIPTEN_KEEPALIVE unsigned char *pg_frame(void)
{
    return frame;
}

/* bytes written by this call, or < 0 */
EMSCRIPTEN_KEEPALIVE int pg_encode(int pts)
{
    n265_picture pic = {
        .plane = { frame, frame + fw * fh, frame + fw * fh * 5 / 4 },
        .stride = { fw, fw / 2, fw / 2 },
        .pts = pts,
    };
    n265_nal *nal;
    int nnal;
    int r = n265_encoder_encode(enc, &nal, &nnal, &pic);
    return write_nals(r, nal, nnal);
}

/* one flush step: bytes written, 0 once drained, or < 0 */
EMSCRIPTEN_KEEPALIVE int pg_flush(void)
{
    n265_nal *nal;
    int nnal;
    int r = n265_encoder_encode(enc, &nal, &nnal, NULL);
    return write_nals(r, nal, nnal);
}

/* closes the files so /out.hevc and /recon.yuv are complete */
EMSCRIPTEN_KEEPALIVE double pg_close(void)
{
    double total = (double)nbytes;
    pg_free();
    return total;
}

EMSCRIPTEN_KEEPALIVE const char *pg_version(void)
{
    return n265_version_str();
}
