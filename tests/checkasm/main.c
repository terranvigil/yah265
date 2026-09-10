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

#include <time.h>

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
        printf("    %-22s ref %8.1f   c %8.1f   neon %8.1f ns   %5.2fx\n",
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

/* ---- the table ------------------------------------------------------ */

typedef struct kernel_test {
    const char *name;
    int (*run)(void);   /* 0 = pass */
} kernel_test;

/* one line per family file; adding a family touches this file and
 * tests/meson.build, and nothing else */
int check_transform(void);
int check_satd(void);
int check_mc(void);

static const kernel_test tests[] = {
    { "transform", check_transform },
    { "satd", check_satd },
    { "mc", check_mc },
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
        printf("checkasm: cpu %s\n", name);
    }
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
