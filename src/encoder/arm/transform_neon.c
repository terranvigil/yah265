/*
 * transform_neon.c - NEON forward/inverse transform kernels (class
 * N265_ASM_DCT). One file per kernel family; see src/encoder/arm/neon.h for
 * the dispatch convention every kernel here follows.
 *
 * Empty on purpose: the transform family item fills it. It exists now so
 * meson.build is written once and the family worktrees never merge-conflict
 * on the build file.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_NEON

#include <arm_neon.h>

#include "encoder/arm/neon.h"

#else
typedef int n265_transform_neon_unused_;  /* C wants a non-empty TU */
#endif /* N265_NEON */
