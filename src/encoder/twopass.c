/*
 * Two-pass rate control: the stats sidecar and the rate-factor solve.
 * See twopass.h for the model and for why the solve is closed form.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdlib.h>
#include <string.h>

#include "encoder/twopass.h"

void n265_2p_init(n265_2p *tp)
{
    memset(tp, 0, sizeof *tp);
}

void n265_2p_free(n265_2p *tp)
{
    if (tp->out)
        fclose(tp->out);
    free(tp->out_path);
    free(tp->out_tmp);
    free(tp->fr);
    free(tp->disp);
    memset(tp, 0, sizeof *tp);
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

int n265_2p_open_write(n265_2p *tp, const char *path, int rewrite,
                       int width, int height, int fps_num, int fps_den,
                       int nblk, const char *ver)
{
    const char *target = path;
    if (rewrite) {
        size_t n = strlen(path) + 5;
        tp->out_tmp = malloc(n);
        if (!tp->out_tmp)
            return -1;
        snprintf(tp->out_tmp, n, "%s.tmp", path);
        tp->out_path = dupstr(path);
        if (!tp->out_path)
            return -1;
        target = tp->out_tmp;
    }
    tp->out = fopen(target, "w");
    if (!tp->out) {
        fprintf(stderr, "n265: cannot write --stats %s\n", target);
        return -1;
    }
    /* The header is machine-read for exactly one field - `nblk`, which the
     * complexity normalisation divides by - and human-read for the rest.
     * Everything a reader might want to know about which encode produced
     * the file is on it, because the failure this guards against is a plan
     * built from somebody else's numbers. */
    fprintf(tp->out, "#n265-2pass %d\n", N265_2P_VER);
    fprintf(tp->out, "#geom w=%d h=%d fps=%d/%d nblk=%d\n",
            width, height, fps_num, fps_den, nblk);
    fprintf(tp->out, "#enc %s\n", ver ? ver : "");
    fprintf(tp->out, "#cols coding display type nonref flat qp bits cplx "
                     "icost pcost prop ctf\n");
    return 0;
}

void n265_2p_write(n265_2p *tp, const n265_2p_frame *f)
{
    if (!tp->out)
        return;
    /* `qp` carries six decimals because the base is fractional under ABR
     * (N265_QP_FRAC) and the whole anchor is `bits x qscale(qp)`: a
     * hundredth of a QP is a sixth of a percent of the frame's bits, which
     * is larger than the rate accuracy this item is gated on. */
    fprintf(tp->out, "%d %d %d %d %d %.6f %.0f %.0f %.0f %.0f %.3f %.6f\n",
            f->coding, f->display, f->slice_type, f->nonref, f->flat,
            f->qp, f->bits, f->cplx, f->icost, f->pcost, f->prop, f->ctf);
}

void n265_2p_close_write(n265_2p *tp)
{
    if (!tp->out)
        return;
    fflush(tp->out);
    fclose(tp->out);
    tp->out = NULL;
    /* pass 3: the file the next pass reads appears whole or not at all */
    if (tp->out_tmp && tp->out_path)
        rename(tp->out_tmp, tp->out_path);
}

static int grow(n265_2p *tp)
{
    if (tp->n < tp->cap)
        return 0;
    int cap = tp->cap ? tp->cap * 2 : 256;
    n265_2p_frame *p = realloc(tp->fr, (size_t)cap * sizeof *p);
    if (!p)
        return -1;
    tp->fr = p;
    tp->cap = cap;
    return 0;
}

int n265_2p_read(n265_2p *tp, const char *path, char *err, size_t errn)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errn, "cannot read %s", path);
        return -1;
    }
    char line[512];
    int ver = -1, ln = 0;
    tp->nblk = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (line[0] == '#') {
            int v;
            if (sscanf(line, "#n265-2pass %d", &v) == 1)
                ver = v;
            else
                sscanf(line, "#geom w=%d h=%d fps=%*d/%*d nblk=%d",
                       &tp->width, &tp->height, &tp->nblk);
            continue;
        }
        if (line[0] == '\n' || line[0] == '\0')
            continue;
        if (ver != N265_2P_VER) {
            snprintf(err, errn, "%s: not an n265 two-pass stats file of "
                     "version %d (read %d)", path, N265_2P_VER, ver);
            fclose(f);
            return -1;
        }
        if (grow(tp) < 0) {
            snprintf(err, errn, "out of memory reading %s", path);
            fclose(f);
            return -1;
        }
        n265_2p_frame *r = &tp->fr[tp->n];
        memset(r, 0, sizeof *r);
        int got = sscanf(line, "%d %d %d %d %d %lf %lf %lf %lf %lf %lf %lf",
                         &r->coding, &r->display, &r->slice_type, &r->nonref,
                         &r->flat, &r->qp, &r->bits, &r->cplx, &r->icost,
                         &r->pcost, &r->prop, &r->ctf);
        if (got != 12) {
            snprintf(err, errn, "%s:%d: %d of 12 columns", path, ln, got);
            fclose(f);
            return -1;
        }
        if (r->display < 0) {
            snprintf(err, errn, "%s:%d: display index %d", path, ln,
                     r->display);
            fclose(f);
            return -1;
        }
        tp->n++;
    }
    fclose(f);
    if (tp->n == 0) {
        snprintf(err, errn, "%s: no frames", path);
        return -1;
    }
    if (tp->nblk <= 0) {
        snprintf(err, errn, "%s: header carries no lowres block count", path);
        return -1;
    }
    /* The display index is what pass 2 looks a frame up by, and it does so
     * because the two passes need not agree on CODING order: a pass 1 at a
     * different preset can pick different mini-GOP lengths, and a plan
     * indexed by coding position would then be silently misaligned rather
     * than visibly short. Display order is the one order both passes see
     * the same way, because it is the input's. */
    int hi = 0;
    for (int i = 0; i < tp->n; i++)
        if (tp->fr[i].display + 1 > hi)
            hi = tp->fr[i].display + 1;
    tp->disp = malloc((size_t)hi * sizeof *tp->disp);
    if (!tp->disp) {
        snprintf(err, errn, "out of memory indexing %s", path);
        return -1;
    }
    for (int i = 0; i < hi; i++)
        tp->disp[i] = -1;
    for (int i = 0; i < tp->n; i++)
        if (tp->disp[tp->fr[i].display] < 0)
            tp->disp[tp->fr[i].display] = i;
    tp->ndisp = hi;
    return tp->n;
}

const n265_2p_frame *n265_2p_get(const n265_2p *tp, int d)
{
    if (!tp->solved || d < 0 || d >= tp->ndisp)
        return NULL;
    int i = tp->disp[d];
    return i < 0 ? NULL : &tp->fr[i];
}

int n265_2p_solve(n265_2p *tp, double target, double qcomp, double clamp,
                  double qp_min, double qp_max)
{
    if (tp->n <= 0 || !(target > 0.0))
        return -1;
    int nblk = tp->nblk > 0 ? tp->nblk : 1;
    /* The yardstick is the GEOMETRIC mean of the per-block complexity,
     * which is the mean the offset is linear in: d_i is a difference of
     * log2s, so the geometric mean is the unique level at which the
     * offsets sum to zero and the plan is a pure re-allocation. An
     * arithmetic mean would put a standing offset on the whole file that
     * the rate-factor solve would then take straight back out - the same
     * quantity twice, once as an allocation and once as a level. */
    double lsum = 0.0;
    for (int i = 0; i < tp->n; i++) {
        double c = tp->fr[i].cplx / (double)nblk;
        if (!(c > 1e-9))
            c = 1e-9;
        lsum += log2(c);
    }
    double lbar = lsum / (double)tp->n;
    for (int i = 0; i < tp->n; i++) {
        n265_2p_frame *r = &tp->fr[i];
        r->x = r->bits * n265_2p_qscale(r->qp);
        /* A coded picture is never free - a slice header alone is tens of
         * bits - so a non-positive anchor is a corrupt line, not a frame,
         * and the floor only keeps the solve finite. */
        if (!(r->x > 0.0))
            r->x = 1.0;
        double c = r->cplx / (double)nblk;
        if (!(c > 1e-9))
            c = 1e-9;
        double d = 6.0 * (1.0 - qcomp) * (log2(c) - lbar);
        if (d > clamp)
            d = clamp;
        else if (d < -clamp)
            d = -clamp;
        r->d = d;
        r->pinned = 0;
        r->plan_qp = 0.0;
        r->plan_bits = 0.0;
    }
    tp->qcomp = qcomp;
    tp->clamp = clamp;
    tp->target_bits = target;
    /* The closed-form solve, with the frames the QP RANGE pins taken out
     * and their bits taken off the target. A frame never unpins - pinning
     * one can only raise the rate factor asked of the rest, which can only
     * pin more - so the walk terminates, and 32 is a bound on it rather
     * than a tolerance. */
    double fixed_bits = 0.0;
    double rf = 0.0;
    for (int it = 0; it < 32; it++) {
        double S = 0.0;
        for (int i = 0; i < tp->n; i++)
            if (!tp->fr[i].pinned)
                S += tp->fr[i].x * exp2(-tp->fr[i].d / 6.0);
        if (!(S > 0.0))
            break;                      /* every frame is at a bound */
        double rem = target - fixed_bits;
        if (rem < 1.0)
            rem = 1.0;
        rf = n265_2p_qp(S / rem);
        if (rf < qp_min)
            rf = qp_min;
        else if (rf > qp_max)
            rf = qp_max;
        int moved = 0;
        for (int i = 0; i < tp->n; i++) {
            n265_2p_frame *r = &tp->fr[i];
            if (r->pinned)
                continue;
            double q = rf + r->d;
            if (q < qp_min || q > qp_max) {
                r->plan_qp = q < qp_min ? qp_min : qp_max;
                r->pinned = 1;
                moved = 1;
            }
        }
        if (!moved)
            break;
        fixed_bits = 0.0;
        for (int i = 0; i < tp->n; i++)
            if (tp->fr[i].pinned)
                fixed_bits += tp->fr[i].x / n265_2p_qscale(tp->fr[i].plan_qp);
    }
    tp->rf = rf;
    tp->plan_bits = 0.0;
    for (int i = 0; i < tp->n; i++) {
        n265_2p_frame *r = &tp->fr[i];
        if (!r->pinned)
            r->plan_qp = rf + r->d;
        r->plan_bits = r->x / n265_2p_qscale(r->plan_qp);
        tp->plan_bits += r->plan_bits;
    }
    tp->solved = 1;
    return 0;
}
