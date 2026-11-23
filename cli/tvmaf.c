/*
 * Target-quality encoding (--target-vmaf): the CRF search and the VMAF
 * scorer.
 *
 * The search walks a CRF grid. It keeps the worst-quality point known to
 * meet the target (good) and the best-quality point known to miss it
 * (bad). Before both exist it steps from the frontier by a measured or
 * prior slope, doubling its cap each step; once they do it takes secant
 * steps between them in logit(VMAF), where the curve is close to linear,
 * and bisects when one side keeps moving. Every probe lands strictly
 * inside the bracket or beyond the only known side, so no point is
 * scored twice and the walk ends. It is dynamic-crf's search, in C.
 *
 * The scorer is scripts/parity_long.py's VMAF v1 chain run as child
 * processes (ffmpeg decode, ffmpeg conversion, libvmaf's tool), or on a
 * PQ/HLG stream scripts/hdr_bd.py's HDR-VMAF chain.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#include "tvmaf.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* ---- the search ---- */

#define TV_MAXV 100.0
#define TV_MIN_SLOPE (-0.02)    /* flatter is noise or saturation */
#define TV_PLATEAU_VMAF 0.1
#define TV_PLATEAU_RATE 1.02
#define TV_VERIFY_STEPS 3

void tv_params_default(tv_params *p)
{
    p->target = 93.0;
    p->tol = 0.5;
    p->crf_lo = 8.0;
    p->crf_hi = 45.0;
    p->crf_start = 26.0;
    p->step = 0.25;
    p->prior_slope = -0.2;
    p->floor = 0;
}

const char *tv_status_name(tv_status s)
{
    static const char *const n[] = {
        "converged", "bracketed",
        "target exceeded at the worst-quality bound",
        "target unreachable at the best-quality bound",
        "plateau: quality stopped rising as the CRF fell",
    };
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

typedef struct {
    const tv_params *p;
    tv_score_fn fn;
    void *ctx;
    int n;
    tv_score *sc;
    char *seen;
    int trials;
} tv_grid;

static double g_crf(const tv_grid *g, int i)
{
    return round((g->p->crf_lo + i * g->p->step) * 1e6) / 1e6;
}

static int g_eval(tv_grid *g, int i, tv_score *out)
{
    if (!g->seen[i]) {
        if (g->fn(g->ctx, g_crf(g, i), &g->sc[i]) < 0)
            return -1;
        g->seen[i] = 1;
        g->trials++;
    }
    *out = g->sc[i];
    return 0;
}

static double g_logit(double v)
{
    double c = fmin(fmax(v, 1.0), TV_MAXV - 0.1);
    return log(c / (TV_MAXV - c));
}

static int g_accept(const tv_grid *g, double v)
{
    double t = g->p->target;
    return g->p->floor ? v >= t && v < t + g->p->tol : fabs(v - t) < g->p->tol;
}

/* d logit / d index from two scored points, or the prior */
static double g_slope(const tv_grid *g, int a, int b)
{
    double prior = g->p->prior_slope * g->p->step;
    if (a < 0 || b < 0 || a == b)
        return prior;
    double k = (g_logit(g->sc[b].vmaf) - g_logit(g->sc[a].vmaf)) / (b - a);
    return k > TV_MIN_SLOPE * g->p->step ? prior : k;
}

static int g_secant(const tv_grid *g, int good, int bad, int same_side)
{
    double yg = g_logit(g->sc[good].vmaf), yb = g_logit(g->sc[bad].vmaf);
    int next = (good + bad) / 2;
    if (same_side < 3 && yg > yb) {
        double root = good + (yg - g_logit(g->p->target)) / (yg - yb) *
                             (bad - good);
        next = (int)floor(root);
    }
    if (next < good + 1)
        next = good + 1;
    if (next > bad - 1)
        next = bad - 1;
    return next;
}

static int g_plateau(const tv_grid *g, int bad, int bad2)
{
    if (bad2 < 0)
        return 0;
    const tv_score *hi = &g->sc[bad], *lo = &g->sc[bad2];
    if (hi->kbps <= 0 || lo->kbps <= 0)
        return 0;
    return hi->kbps <= TV_PLATEAU_RATE * lo->kbps &&
           hi->vmaf - lo->vmaf < TV_PLATEAU_VMAF;
}

static int g_finish(tv_grid *g, int i, tv_status st, tv_result *out)
{
    tv_score s;
    if (g_eval(g, i, &s) < 0)
        return -1;
    /* floor: step toward quality while the pick is under target */
    if (g->p->floor && st != TV_UNREACHABLE && st != TV_PLATEAU &&
        s.vmaf < g->p->target) {
        for (int k = 0; s.vmaf < g->p->target && k < TV_VERIFY_STEPS && i > 0;
             k++)
            if (g_eval(g, --i, &s) < 0)
                return -1;
        st = g_accept(g, s.vmaf) ? TV_CONVERGED
           : s.vmaf >= g->p->target ? TV_BRACKETED
           : i == 0 ? TV_UNREACHABLE : TV_BRACKETED;
    }
    out->crf = g_crf(g, i);
    out->s = s;
    out->status = st;
    out->trials = g->trials;
    return 0;
}

static int g_index(const tv_grid *g, double crf)
{
    int i = (int)lround((crf - g->p->crf_lo) / g->p->step);
    return i < 0 ? 0 : i > g->n ? g->n : i;
}

int tv_search(const tv_params *p, tv_score_fn fn, void *ctx, tv_result *out)
{
    if (p->step <= 0 || p->crf_hi - p->crf_lo < p->step || p->tol <= 0 ||
        p->target <= 0 || p->target >= TV_MAXV)
        return -1;
    tv_grid g = { .p = p, .fn = fn, .ctx = ctx };
    g.n = (int)floor((p->crf_hi - p->crf_lo) / p->step + 1e-9);
    g.sc = calloc((size_t)g.n + 1, sizeof *g.sc);
    g.seen = calloc((size_t)g.n + 1, 1);
    if (!g.sc || !g.seen) {
        free(g.sc);
        free(g.seen);
        return -1;
    }
    int rc = -1;
    int i = g_index(&g, p->crf_start);
    int good = -1, bad = -1, good2 = -1, bad2 = -1;
    int last_side = 0, same_side = 0, cap = g.n;
    const double yt = g_logit(p->target);

    for (int it = 0; it < g.n + 2; it++) {
        if (g.seen[i])
            break;              /* cannot happen; refuse rather than loop */
        tv_score s;
        if (g_eval(&g, i, &s) < 0)
            goto done;
        if (g_accept(&g, s.vmaf)) {
            rc = g_finish(&g, i, TV_CONVERGED, out);
            goto done;
        }
        int side = -1;
        if (s.vmaf >= p->target) {
            side = 1;
            if (i > good) {
                good2 = good;
                good = i;
            } else if (i > good2) {
                good2 = i;
            }
        } else if (bad < 0 || i < bad) {
            bad2 = bad;
            bad = i;
        } else if (bad2 < 0 || i < bad2) {
            bad2 = i;
        }
        same_side = side == last_side ? same_side + 1 : 1;
        last_side = side;

        if (good >= 0 && bad >= 0) {
            if (bad - good <= 1) {
                int pick = good;
                if (!p->floor && fabs(g.sc[bad].vmaf - p->target) <
                                 fabs(g.sc[good].vmaf - p->target))
                    pick = bad;
                rc = g_finish(&g, pick, TV_BRACKETED, out);
                goto done;
            }
            i = g_secant(&g, good, bad, same_side);
        } else if (good >= 0) {
            /* everything so far meets the target: trade quality for size */
            if (good == g.n) {
                rc = g_finish(&g, g.n, TV_EXCEEDED, out);
                goto done;
            }
            double d = (yt - g_logit(g.sc[good].vmaf)) / g_slope(&g, good2, good);
            int step = (int)floor(d);
            step = step < 1 ? 1 : step > cap ? cap : step;
            cap = 2 * step;
            i = good + step > g.n ? g.n : good + step;
        } else {
            /* everything so far misses: spend bits */
            if (bad == 0) {
                rc = g_finish(&g, 0, TV_UNREACHABLE, out);
                goto done;
            }
            if (g_plateau(&g, bad, bad2)) {
                rc = g_finish(&g, bad, TV_PLATEAU, out);
                goto done;
            }
            double d = (yt - g_logit(g.sc[bad].vmaf)) / -g_slope(&g, bad, bad2);
            int step = (int)ceil(d);
            step = step < 1 ? 1 : step > cap ? cap : step;
            cap = 2 * step;
            i = bad - step < 0 ? 0 : bad - step;
        }
    }
done:
    free(g.sc);
    free(g.seen);
    return rc;
}

/* ---- the scorer ---- */

static const char *const hlg_to_pq =
    "zscale=tin=arib-std-b67:min=bt2020nc:pin=bt2020:rin=tv:t=linear:"
    "npl=1000:p=bt2020,format=gbrpf32le,zscale=t=smpte2084:"
    "m=bt2020nc:r=tv:p=bt2020,format=yuv420p10le";

int tv_upscaled(int w, int h)
{
    return w < 1920 && h < 1080;
}

static int above_1080p(int h)
{
    return h > 1080;
}

const char *tv_model_name(const tv_scorer *s)
{
    static char buf[64];
    int hfr = s->fps >= 49.9;
    snprintf(buf, sizeof buf, "%s%s_%s",
             s->hdr ? "hdr_vmaf_v1.1.8" : "vmaf_v1.0.16", hfr ? "_hfr" : "",
             above_1080p(s->h) ? "1d5h_2160" : "3d0h");
    return buf;
}

int tv_model_path(const tv_scorer *s, char *buf, size_t n)
{
    int hfr = s->fps >= 49.9;
    const char *dir = s->hdr ? "hdr_vmaf_v1.1.8" : "vmaf_v1.0.16";
    int k = snprintf(buf, n, "%s/%s%s/%s.json", s->model_dir, dir,
                     hfr ? "_hfr" : "", tv_model_name(s));
    return k > 0 && (size_t)k < n ? 0 : -1;
}

static pid_t spawn(char *const argv[], int in_fd, int out_fd, const char *err)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in_fd >= 0)
        posix_spawn_file_actions_adddup2(&fa, in_fd, 0);
    else
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out_fd >= 0)
        posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
    else
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, err, O_WRONLY | O_CREAT | O_TRUNC,
                                     0600);
    pid_t pid;
    int r = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r) {
        fprintf(stderr, "y265: target-vmaf: cannot run %s: %s\n", argv[0],
                strerror(r));
        return -1;
    }
    return pid;
}

static int reap(pid_t pid)
{
    int st;
    if (pid <= 0)
        return -1;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int cloexec_pipe(int fd[2])
{
    if (pipe(fd) < 0)
        return -1;
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    if (b)
        b[n] = 0;
    fclose(f);
    return b;
}

static void tell_err(const char *what, const char *errpath)
{
    char *e = slurp(errpath);
    size_t n = e ? strlen(e) : 0;
    fprintf(stderr, "y265: target-vmaf: %s failed%s%s", what,
            n ? ":\n" : "\n", n > 600 ? e + n - 600 : n ? e : "");
    free(e);
}

int tv_score_stream(const tv_scorer *s, const char *wd, const char *bitstream,
                    tv_ref_fn ref, void *ref_ctx, double *vmaf, long *frames)
{
    char dfifo[1024], json[1024], e_dec[1024], e_conv[1024], e_vmaf[1024];
    char model[1024], marg[1400], thr[16], scale[160];
    snprintf(dfifo, sizeof dfifo, "%s/dist.fifo", wd);
    snprintf(json, sizeof json, "%s/score.json", wd);
    snprintf(e_dec, sizeof e_dec, "%s/decode.err", wd);
    snprintf(e_conv, sizeof e_conv, "%s/convert.err", wd);
    snprintf(e_vmaf, sizeof e_vmaf, "%s/vmaf.err", wd);
    if (tv_model_path(s, model, sizeof model) < 0)
        return -1;
    snprintf(thr, sizeof thr, "%d", s->threads > 0 ? s->threads : 1);
    if (s->hdr)
        snprintf(marg, sizeof marg, "path=%s:name=tvmaf", model);
    else
        snprintf(marg, sizeof marg, "path=%s:name=tvmaf:cambi.enc_bitdepth=%d"
                 ":cambi.enc_width=%d:cambi.enc_height=%d", model,
                 s->enc_depth, s->w, s->h);
    const char *vf = NULL;
    if (s->hdr && s->hlg) {
        vf = hlg_to_pq;
    } else if (!s->hdr && tv_upscaled(s->w, s->h)) {
        snprintf(scale, sizeof scale, "scale=1920:1080:flags=bicubic"
                 ":force_original_aspect_ratio=decrease:force_divisible_by=2");
        vf = scale;
    }

    unlink(dfifo);
    unlink(json);
    if (mkfifo(dfifo, 0600) < 0) {
        fprintf(stderr, "y265: target-vmaf: mkfifo %s: %s\n", dfifo,
                strerror(errno));
        return -1;
    }
    char *ff = (char *)s->ffmpeg_bin;
    char *dec[24], *conv[24], *vm[40];
    int k = 0;
    dec[k++] = ff; dec[k++] = "-v"; dec[k++] = "error"; dec[k++] = "-nostdin";
    dec[k++] = "-y"; dec[k++] = "-threads"; dec[k++] = "2";
    dec[k++] = "-i"; dec[k++] = (char *)bitstream;
    if (vf) { dec[k++] = "-vf"; dec[k++] = (char *)vf; }
    dec[k++] = "-pix_fmt"; dec[k++] = "yuv420p10le"; dec[k++] = "-strict";
    dec[k++] = "-1"; dec[k++] = "-f"; dec[k++] = "yuv4mpegpipe";
    dec[k++] = dfifo; dec[k] = NULL;
    k = 0;
    conv[k++] = ff; conv[k++] = "-v"; conv[k++] = "error";
    conv[k++] = "-f"; conv[k++] = "yuv4mpegpipe"; conv[k++] = "-i";
    conv[k++] = "-";
    if (vf) { conv[k++] = "-vf"; conv[k++] = (char *)vf; }
    conv[k++] = "-pix_fmt"; conv[k++] = "yuv420p10le"; conv[k++] = "-strict";
    conv[k++] = "-1"; conv[k++] = "-f"; conv[k++] = "yuv4mpegpipe";
    conv[k++] = "-"; conv[k] = NULL;
    k = 0;
    vm[k++] = (char *)s->vmaf_bin; vm[k++] = "--threads"; vm[k++] = thr;
    vm[k++] = "-r"; vm[k++] = "/dev/stdin"; vm[k++] = "-d"; vm[k++] = dfifo;
    vm[k++] = "--json"; vm[k++] = "-o"; vm[k++] = json;
    vm[k++] = "--model"; vm[k++] = marg;
    if (s->hdr) {
        static char *const col[] = {
            "--color_range_ref", "limited", "--color_primaries_ref", "bt2020",
            "--color_trc_ref", "pq", "--color_matrix_ref", "bt2020nc",
            "--color_range_dist", "limited", "--color_primaries_dist",
            "bt2020", "--color_trc_dist", "pq", "--color_matrix_dist",
            "bt2020nc",
        };
        for (size_t c = 0; c < sizeof col / sizeof col[0]; c++)
            vm[k++] = col[c];
    }
    vm[k] = NULL;

    int rp[2] = { -1, -1 }, vp[2] = { -1, -1 };
    pid_t pd = -1, pc = -1, pv = -1;
    int rc = -1, wrote = -1;
    if (cloexec_pipe(rp) < 0 || cloexec_pipe(vp) < 0)
        goto out;
    pv = spawn(vm, vp[0], -1, e_vmaf);
    pc = spawn(conv, rp[0], vp[1], e_conv);
    pd = spawn(dec, -1, -1, e_dec);
    close(rp[0]);
    close(vp[0]);
    close(vp[1]);
    rp[0] = vp[0] = vp[1] = -1;
    if (pv < 0 || pc < 0 || pd < 0) {
        close(rp[1]);
        rp[1] = -1;
        goto out;
    }
    {
        void (*old)(int) = signal(SIGPIPE, SIG_IGN);
        FILE *f = fdopen(rp[1], "wb");
        if (f) {
            rp[1] = -1;
            wrote = ref(ref_ctx, f);
            if (fclose(f) != 0)
                wrote = -1;
        }
        signal(SIGPIPE, old);
    }
out:
    if (rp[1] >= 0)
        close(rp[1]);
    {
        int rv = reap(pv), rcv = reap(pc);
        if (pd > 0 && rv < 0)
            kill(pd, SIGTERM);
        int rd = reap(pd);
        if (pv > 0 && rv < 0)
            tell_err(s->vmaf_bin, e_vmaf);
        else if (pc > 0 && rcv < 0)
            tell_err("the reference conversion (ffmpeg)", e_conv);
        else if (pd > 0 && rd < 0)
            tell_err("the decode (ffmpeg)", e_dec);
        else if (pv > 0 && wrote == 0)
            rc = 0;
    }
    unlink(dfifo);
    unlink(e_dec);
    unlink(e_conv);
    unlink(e_vmaf);
    if (rc < 0)
        return -1;

    char *j = slurp(json);
    unlink(json);
    const char *pm = j ? strstr(j, "\"pooled_metrics\"") : NULL;
    const char *m = pm ? strstr(pm, "\"tvmaf\"") : NULL;
    const char *mean = m ? strstr(m, "\"mean\"") : NULL;
    const char *colon = mean ? strchr(mean, ':') : NULL;
    if (!colon) {
        fprintf(stderr, "y265: target-vmaf: no pooled score in %s's output\n",
                s->vmaf_bin);
        free(j);
        return -1;
    }
    *vmaf = strtod(colon + 1, NULL);
    long nf = 0;
    for (const char *q = j; (q = strstr(q, "\"frameNum\"")); q++)
        nf++;
    *frames = nf;
    free(j);
    return 0;
}
