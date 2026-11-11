/*
 * checkasm: validates every dispatched kernel against its C reference and
 * benchmarks it. "Dispatched" includes a C kernel that replaced a slower
 * C one: the reference stays in the tree and the fast path is checked
 * against it here, on random and on adversarial input, before it merges.
 *
 * The driver, the shared harness and the CLI. Per-family checks live in
 * their own files; see checkasm.h for the layout and how to add one.
 *
 *   checkasm                 correctness only, every family (what `make
 *                            test` runs, and it stays fast)
 *   checkasm --bench         correctness, then the benchmark tables
 *   checkasm [--bench] mc    one family
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "checkasm.h"

#include "common/cpu.h"
#include "encoder/tables.h"

int ca_bench;

static uint32_t rs = 0x2545f491u;

uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

void bench(const char *what, void (*ref)(void *), void (*opt)(void *),
           void (*neon)(void *), void *ctx, int iters)
{
    double t0, t1, t2, t3 = 0;

    if (!ca_bench)
        return;
    for (int i = 0; i < iters / 8; i++) {
        ref(ctx);
        opt(ctx);
        if (neon)
            neon(ctx);
    }
    t0 = now_ns();
    for (int i = 0; i < iters; i++) ref(ctx);
    t1 = now_ns();
    for (int i = 0; i < iters; i++) opt(ctx);
    t2 = now_ns();
    if (neon) {
        for (int i = 0; i < iters; i++) neon(ctx);
        t3 = now_ns();
        printf("    %-22s ref %8.1f   c %8.1f   " CA_ISA
               " %8.1f ns   %5.2fx\n",
               what, (t1 - t0) / iters, (t2 - t1) / iters,
               (t3 - t2) / iters, (t2 - t1) / (t3 - t2));
        return;
    }
    printf("    %-22s ref %8.1f ns   opt %8.1f ns   %5.2fx\n", what,
           (t1 - t0) / iters, (t2 - t1) / iters, (t1 - t0) / (t2 - t1));
}

/* ---- guard buffers -------------------------------------------------- */

void *ca_guard_alloc(size_t bytes)
{
    uint8_t *base = malloc(bytes + 2 * CA_GUARD);
    if (!base) {
        fprintf(stderr, "checkasm: out of memory\n");
        exit(2);
    }
    memset(base, CA_POISON, bytes + 2 * CA_GUARD);
    return base + CA_GUARD;
}

void ca_guard_free(void *blk)
{
    if (blk)
        free((uint8_t *)blk - CA_GUARD);
}

void ca_guard_poison(void *blk, size_t bytes)
{
    uint8_t *p = blk;
    memset(p - CA_GUARD, CA_POISON, CA_GUARD);
    memset(p + bytes, CA_POISON, CA_GUARD);
}

void ca_guard_scramble(void *blk, size_t bytes)
{
    uint8_t *p = blk;
    for (int i = 0; i < CA_GUARD; i++) {
        p[-CA_GUARD + i] = (uint8_t)(rnd() >> 24);
        p[bytes + i] = (uint8_t)(rnd() >> 24);
    }
}

int ca_guard_check(const void *blk, size_t bytes, const char *what)
{
    const uint8_t *p = blk;
    for (int i = 0; i < CA_GUARD; i++) {
        if (p[-CA_GUARD + i] != CA_POISON) {
            printf("    %s: wrote %d byte(s) BEFORE the block\n", what,
                   CA_GUARD - i);
            return 1;
        }
        if (p[bytes + i] != CA_POISON) {
            printf("    %s: wrote %d byte(s) PAST the block\n", what, i + 1);
            return 1;
        }
    }
    return 0;
}

int ca_check_pad(const uint8_t *blk, int stride, int w, int h,
                 const char *what)
{
    for (int y = 0; y < h; y++)
        for (int x = w; x < stride; x++)
            if (blk[(ptrdiff_t)y * stride + x] != CA_POISON) {
                printf("    %s: wrote into the row padding at (%d,%d)\n",
                       what, x, y);
                return 1;
            }
    return 0;
}

/* ---- the page guard -------------------------------------------------- */

static char ca_pg_what[96];

static void ca_pg_fault(int sig)
{
    (void)sig;
    /* async-signal-safe: write(2) and _exit(2) only */
    static const char msg[] =
        "    checkasm: READ OUTSIDE THE DECLARED WINDOW: ";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    r += write(2, ca_pg_what, strlen(ca_pg_what));
    r += write(2, "\n", 1);
    (void)r;
    _exit(1);
}

void *ca_pg_alloc(ca_pg *g, size_t bytes, int tail)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    size_t body = (bytes + pg - 1) / pg * pg;

    g->maplen = body + 2 * pg;
    g->map = mmap(NULL, g->maplen, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g->map == MAP_FAILED) {
        printf("    checkasm: mmap failed\n");
        exit(2);
    }
    if (mprotect(g->map, pg, PROT_NONE) ||
        mprotect(g->map + pg + body, pg, PROT_NONE)) {
        printf("    checkasm: mprotect failed\n");
        exit(2);
    }
    g->user = g->map + pg + (tail ? body - bytes : 0);
    return g->user;
}

void ca_pg_free(ca_pg *g)
{
    munmap(g->map, g->maplen);
}

void ca_pg_fill(ca_pg *g, size_t bytes)
{
    uint8_t *p = g->user;
    for (size_t i = 0; i < bytes; i++)
        p[i] = (uint8_t)(rnd() >> 24);
}

void ca_pg_arm(const char *what)
{
    snprintf(ca_pg_what, sizeof(ca_pg_what), "%s", what);
    signal(SIGSEGV, ca_pg_fault);
    signal(SIGBUS, ca_pg_fault);
}

void ca_pg_disarm(void)
{
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

/* ---- the table ------------------------------------------------------ */

typedef struct kernel_test {
    const char *name;
    int (*run)(void);   /* 0 = pass */
} kernel_test;

/* One line per family file; adding a family touches this file and
 * tests/meson.build, and nothing else.
 *
 * Which files are IN this binary is a per-DEPTH question since phase 8
 * item 3: `pixel` is uint8_t at 8 bits and uint16_t at Main10, and a
 * family reaches a depth only once its lanes are proved there, so at 10
 * bits the table holds exactly the families that have 16-bit kernels.
 * tests/meson.build passes one N265_CA_<FAMILY> per file it compiles. A
 * family whose file is absent is absent from the table too, rather than
 * reporting an "ok" that ran nothing. */
#if N265_CA_TRANSFORM
int check_transform(void);
#endif
#if N265_CA_SATD
int check_satd(void);
#endif
#if N265_CA_MC
int check_mc(void);
#endif
#if N265_CA_RDOQ
int check_rdoq(void);
#endif
#if N265_CA_COST
int check_cost(void);
#endif
#if N265_CA_INTRA
int check_intra(void);
#endif
#if N265_CA_DEBLOCK
int check_deblock(void);
#endif
#if N265_CA_SAO
int check_sao(void);
#endif

static const kernel_test tests[] = {
#if N265_CA_TRANSFORM
    { "transform", check_transform },
#endif
#if N265_CA_SATD
    { "satd", check_satd },
#endif
#if N265_CA_MC
    { "mc", check_mc },
#endif
#if N265_CA_RDOQ
    { "rdoq", check_rdoq },
#endif
#if N265_CA_COST
    { "cost", check_cost },
#endif
#if N265_CA_INTRA
    { "intra", check_intra },
#endif
#if N265_CA_DEBLOCK
    { "deblock", check_deblock },
#endif
#if N265_CA_SAO
    { "sao", check_sao },
#endif
    { NULL, NULL },
};

static void usage(void)
{
    printf("usage: checkasm [--bench] [family]\n"
           "  families:");
    for (const kernel_test *t = tests; t->name; t++)
        printf(" %s", t->name);
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *only = NULL;
    int fails = 0, ran = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench")) {
            ca_bench = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else {
            only = argv[i];
        }
    }
    if (only) {
        int known = 0;
        for (const kernel_test *t = tests; t->name; t++)
            known |= !strcmp(t->name, only);
        if (!known) {
            printf("checkasm: no such family '%s'\n", only);
            usage();
            return 2;
        }
    }

    n265_tables_init();
    {
        char name[64];
        n265_cpu_name(n265_cpu_flags(), name, sizeof(name));
        printf("checkasm: cpu %s, %d-bit samples, checking %s\n", name,
               BIT_DEPTH, CA_ISA);
    }
#if N265_X86
    /* the families call their AVX2 kernels directly, so a CPU without it
     * would fault rather than fail; 77 is meson's skip */
    if (N265_ASM_PORTED_AVX2 && !(n265_cpu_flags() & N265_CPU_AVX2)) {
        printf("checkasm: this CPU does not report AVX2 (under Rosetta, "
               "set ROSETTA_ADVERTISE_AVX=1); skipped\n");
        return 77;
    }
#endif
    for (const kernel_test *t = tests; t->name; t++) {
        int r;
        if (only && strcmp(t->name, only))
            continue;
        r = t->run();
        printf("%-32s %s\n", t->name, r ? "FAIL" : "ok");
        fails += !!r;
        ran++;
    }
    printf("checkasm: %d kernel(s), %d failure(s)\n", ran, fails);
    return fails ? 1 : 0;
}
