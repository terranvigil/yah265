/*
 * yah265: preset tables - the DEFAULT SOURCE for every tuning knob.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_PRESET_H
#define N265_PRESET_H

/* Every N265_* read in the encoder goes through this instead of getenv.
 * It returns the real environment first, and the selected preset's table
 * only where the variable is unset - so an explicit env always wins and
 * a preset can never take a knob away from a measurement. `medium`'s
 * table is empty by construction, which is what makes the default
 * encoder byte-identical to the pre-preset one. */
const char *n265_env(const char *name);

#endif /* N265_PRESET_H */
