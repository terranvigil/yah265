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

/* The selected rung's name, and its table read back row by row (phase 14
 * item 13, for `--show-config`). `n265_preset_row` returns 1 while row
 * `i` exists and hands back the key, the value the table names and the
 * environment's value for that key (NULL where the environment is
 * silent, which is when the table's value is the one in force). */
const char *n265_preset_selected(void);
/* The resolution class n265_preset_uhd latched: 0 below HD, 1 HD-class
 * (luma w*h >= 1920*800), 2 UHD-class (>= 3840*1600). The rate switch
 * reads it to pick its threshold (phase 14 items 17 and 23). */
int n265_preset_res_class(void);
/* The HDR kind, latched by n265_encoder_open before it reads a knob: 0
 * SDR, 1 PQ, 2 HLG. On medium a PQ or HLG kind puts its own table in front
 * of every other (phase 14 item 56). */
void n265_preset_hdr(int kind);
int n265_preset_hdr_kind(void);
/* Capped VBR (a bitrate with a VBV whose maxrate is above it, not strict
 * CBR), latched by n265_encoder_open before it reads a knob. On medium at
 * UHD it puts item 60's rows in front of every other. */
void n265_preset_cvbr(int on);
int n265_preset_row(int i, const char **key, const char **val,
                    const char **env);

/* ---- -t/--tune (phase 14 item 15) -----------------------------------
 *
 * The composition of the one tune this encoder accepts, and the reason
 * for each of the four it refuses. The table lives beside the presets
 * because that is what a tune is - a second name for a set of knob
 * values - and in the library rather than in the CLI so a unit test can
 * read it. `t` is filled by index; -1 is "this tune does not touch that
 * knob", and the caller applies a value only where the command line did
 * not already name one, so an explicit flag beats the tune and the tune
 * beats the preset. */
enum {
    N265_TUNE_BFRAMES = 0,      /* --bframes */
    N265_TUNE_LOOKAHEAD,        /* --rc-lookahead */
    N265_TUNE_BADAPT,           /* --b-adapt */
    N265_TUNE_CUTREE,           /* --cutree */
    /* N265_MAX_FRAME_WORK: frames in flight. A scheduling knob that
     * moves no bits (output is byte-identical at every thread count,
     * which is a gate), so it is in the tune for its LATENCY and not for
     * its rate. */
    N265_TUNE_FRAMEWORK,
    N265_TUNE_NFIELD
};

enum {
    N265_TUNE_OK      =  0,
    N265_TUNE_UNKNOWN = -1,     /* not one of x265's five names */
    N265_TUNE_REFUSED = -2,     /* a name we know and will not do; `why` */
};

int n265_tune_parse(const char *name, int *t, const char **why);
/* Row `i`'s name, NULL past the end: the refusal prints the five without
 * keeping a second copy of them. */
const char *n265_tune_name(int i);

#endif /* N265_PRESET_H */
