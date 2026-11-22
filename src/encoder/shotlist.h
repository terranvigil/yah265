/*
 * The shot list: --shot-scan writes it, --shot-list reads it.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * A list is text, one shot a line, so a ladder tool or a person can write
 * or edit one. The scan adds a fingerprint of the input it read, so a list
 * meant for one source is refused on another. A list without one (written
 * by hand, or by another tool) is checked only against the frame count.
 *
 *   yah265-shots 1
 *   frames 600
 *   source 1920x1080 depth=8
 *   luma 0123456789abcdef
 *   thumb <128 hex digits>          first, middle and last frame
 *   thumb <128 hex digits>
 *   thumb <128 hex digits>
 *   scan 640x360 min-keyint=25
 *   shots 3
 *   0 149 start -
 *   150 299 cut 0.912
 *   300 599 cut 0.884
 *   end
 *
 * A shot line is "first [last [kind [confidence]]]". A hand-written list
 * may give the first frame alone; the last is then the frame before the
 * next shot. Every line but the magic, "shots" and "end" is optional.
 */

#ifndef N265_SHOTLIST_H
#define N265_SHOTLIST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define N265_SHOTLIST_MAGIC "yah265-shots"
#define N265_SHOTLIST_VERSION 1
/* the scan's analysis width: its lowres plane is 320 samples wide, the
 * width the b-adapt motion term is normalized to */
#define N265_SHOTSCAN_W 640

enum { N265_SHOT_START = 0, N265_SHOT_CUT = 1, N265_SHOT_HAND = 2 };

/* What a list knows about the input it was made from. The thumbnails are
 * an 8x8 grid of 8-bit block means, so a rung at another size of the same
 * source matches them where its exact luma hash cannot. */
typedef struct n265_shot_fp {
    int have;
    long frames;
    int w, h, depth;
    uint64_t luma;
    uint8_t thumb[3][64];
} n265_shot_fp;

typedef struct n265_shot {
    int first, last;        /* last is -1 where the list left it open */
    int kind;
    double conf;            /* negative: not given */
} n265_shot;

typedef struct n265_shotlist {
    n265_shot *s;
    int n;
    n265_shot_fp fp;
    int scan_w, scan_h, scan_mk;    /* the scan line, 0 when absent */
} n265_shotlist;

int n265_shotlist_parse_str(n265_shotlist *l, const char *text, char *err,
                            size_t cap);
int n265_shotlist_parse(n265_shotlist *l, const char *path, char *err,
                        size_t cap);
void n265_shotlist_free(n265_shotlist *l);
int n265_shotlist_write(FILE *f, const n265_shotlist *l);

/* 0 when the list may be applied to the input `have` describes; -1 with
 * the reason in `err` otherwise. Closes every open `last` on success. */
int n265_shotlist_check(n265_shotlist *l, const n265_shot_fp *have,
                        char *err, size_t cap);

/* One frame's FNV-1a luma hash and thumbnail, from its luma plane as the
 * file stores it (8-bit bytes, or little-endian 16-bit words). */
void n265_shot_fp_frame(const uint8_t *luma, int w, int h, int depth,
                        uint64_t *hash, uint8_t thumb[64]);
uint64_t n265_shot_fp_combine(const uint64_t h[3]);

/* The analysis height for a source: its aspect at N265_SHOTSCAN_W, even. */
int n265_shotscan_h(int w, int h);

/* Box-resample an input luma plane (as the file stores it) to the 8-bit
 * analysis plane, dw x dh. */
void n265_shotscan_resample(const uint8_t *luma, int w, int h, int depth,
                            uint8_t *dst, int dw, int dh);

#endif
