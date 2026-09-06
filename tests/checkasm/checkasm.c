/*
 * checkasm: validates every dispatched SIMD kernel against its C reference
 * and benchmarks it. No kernels exist yet; the harness registers zero and
 * passes, so the CI wiring is in place the day the first NEON kernel lands.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>

typedef struct kernel_test {
    const char *name;
    int (*run)(void);   /* 0 = pass */
} kernel_test;

static const kernel_test tests[] = {
    { NULL, NULL },
};

int main(void)
{
    int fails = 0, ran = 0;
    for (const kernel_test *t = tests; t->name; t++) {
        int r = t->run();
        printf("%-32s %s\n", t->name, r ? "FAIL" : "ok");
        fails += !!r;
        ran++;
    }
    printf("checkasm: %d kernel(s), %d failure(s)\n", ran, fails);
    return fails ? 1 : 0;
}
