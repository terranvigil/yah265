/*
 * cpu.c - runtime CPU feature detection and N265_ASM_OFF parsing.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "common/cpu.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "encoder/preset.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__APPLE__)
#include <sys/sysctl.h>

static int sysctl_flag(const char *name)
{
    int value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname(name, &value, &len, NULL, 0) != 0)
        return 0;
    return value != 0;
}

static uint32_t detect_arm(void)
{
    uint32_t flags = N265_CPU_NEON;   /* mandatory on aarch64 */
    if (sysctl_flag("hw.optional.arm.FEAT_DotProd"))
        flags |= N265_CPU_DOTPROD;
    if (sysctl_flag("hw.optional.arm.FEAT_I8MM"))
        flags |= N265_CPU_I8MM;
    return flags;
}
#else /* Linux / other aarch64 */
#include <sys/auxv.h>
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)
#endif
#ifndef HWCAP2_I8MM
#define HWCAP2_I8MM (1 << 13)
#endif

static uint32_t detect_arm(void)
{
    uint32_t flags = N265_CPU_NEON;
    unsigned long hw = getauxval(AT_HWCAP);
    unsigned long hw2 = getauxval(AT_HWCAP2);
    if (hw & HWCAP_ASIMDDP)
        flags |= N265_CPU_DOTPROD;
    if (hw2 & HWCAP2_I8MM)
        flags |= N265_CPU_I8MM;
    return flags;
}
#endif
#endif /* aarch64 */

uint32_t n265_cpu_cached_;
uint32_t n265_asm_off_;
uint32_t n265_asm_refuse_;
_Atomic int n265_cpu_ready_;
static pthread_once_t g_cpu_once = PTHREAD_ONCE_INIT;

/* Stand a class down for this encoder's configuration. Driver-only, at
 * open, before any worker exists; it is never cleared, because one
 * encoder per process is what the API promises today. */
void n265_asm_refuse(uint32_t cls)
{
    n265_asm_refuse_ |= cls;
}

static const struct { const char *name; uint32_t bit; } class_tbl[] = {
    { "dct", N265_ASM_DCT },     { "satd", N265_ASM_SATD },
    { "mc", N265_ASM_MC },       { "intra", N265_ASM_INTRA },
    { "sao", N265_ASM_SAO },     { "quant", N265_ASM_QUANT },
    { "all", N265_ASM_ALL },
};

/* comma- or space-separated class list; an unknown name is ignored rather
 * than fatal, so an ablation sweep that outlives a class rename degrades
 * to "nothing ablated" instead of refusing to run */
static uint32_t parse_asm_off(const char *s)
{
    uint32_t off = 0;
    while (*s) {
        while (*s == ',' || *s == ' ')
            s++;
        size_t n = 0;
        while (s[n] && s[n] != ',' && s[n] != ' ')
            n++;
        for (size_t i = 0; i < sizeof(class_tbl) / sizeof(class_tbl[0]); i++)
            if (n == strlen(class_tbl[i].name) &&
                !strncmp(s, class_tbl[i].name, n))
                off |= class_tbl[i].bit;
        s += n;
    }
    return off;
}

/* Both caches are filled before the release store, so the inline fast path
 * in cpu.h can read either one after its acquire load returns nonzero. */
static void cpu_detect_once(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    n265_cpu_cached_ = detect_arm();
#else
    n265_cpu_cached_ = 0;
#endif
    const char *off = n265_env("N265_ASM_OFF");
    n265_asm_off_ = off ? parse_asm_off(off) : 0;
    atomic_store_explicit(&n265_cpu_ready_, 1, memory_order_release);
}

uint32_t n265_cpu_flags_slow(void)
{
    pthread_once(&g_cpu_once, cpu_detect_once);
    return n265_cpu_cached_;
}

void n265_cpu_name(uint32_t flags, char *buf, int size)
{
    static const struct { uint32_t bit; const char *name; } tbl[] = {
        { N265_CPU_NEON, "neon" }, { N265_CPU_DOTPROD, "dotprod" },
        { N265_CPU_I8MM, "i8mm" },
    };
    int off = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (!(flags & tbl[i].bit))
            continue;
        int n = snprintf(buf + off, (size_t)(size - off), "%s%s",
                         off ? " " : "", tbl[i].name);
        if (n < 0 || n >= size - off)
            break;
        off += n;
    }
    if (off == 0)
        snprintf(buf, (size_t)size, "scalar");
}
