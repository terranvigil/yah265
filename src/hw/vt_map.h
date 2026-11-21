/*
 * vt_map.h - what each coding option becomes in the hardware mode
 * (docs/hardware-mode.md). One table, read by the config's warnings, so
 * the warnings and the behaviour can't drift. Data only: built on every
 * platform.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_HW_VT_MAP_H
#define N265_HW_VT_MAP_H

enum n265_hw_class {
    N265_HWC_MAPS = 0,      /* same meaning on the hardware */
    N265_HWC_APPROX,        /* the closest thing the hardware has; warned */
    N265_HWC_IGNORED,       /* no equivalent; warned */
    N265_HWC_FATAL,         /* no hardware path: refused, or software in auto */
};

typedef struct n265_hw_opt {
    const char *name;       /* the option's name without the dashes */
    enum n265_hw_class cls;
    const char *note;
} n265_hw_opt;

/* N265_HWC_MAPS for a name the table doesn't list */
enum n265_hw_class n265_hw_option_class(const char *name, const char **note);
/* terminated by a NULL name */
const n265_hw_opt *n265_hw_option_table(void);

#endif
