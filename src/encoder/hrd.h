/*
 * The HRD timing SEI messages: what buffering_period (D.2.2) and
 * pic_timing (D.2.3) say about one access unit.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * The VUI's hrd_parameters (headers.c, write_hrd) declares a CPB schedule;
 * D.3.2 then asks for a buffering period SEI in every IRAP access unit and
 * a pic_timing in EVERY access unit, and without them the schedule has
 * nothing behind it. This struct is the whole of what those two messages
 * carry for one AU in this encoder's shape - one NAL HRD, one CPB, no
 * sub-picture timing, frame_field_info_present_flag 0 - so the writer is a
 * pure function of it and tests/unit/hrd_test.c can check the syntax
 * without an encoder.
 *
 * Every field is DERIVED FROM THE PLAN and never from a clock or a thread:
 * the coding index, the POC and sps_max_num_reorder_pics are all fixed at
 * dispatch, and the CPB fill is the driver's own coding-order walk. That is
 * what makes the messages byte-identical at one worker and at twelve.
 *
 *   init_delay      90000 * (bits in the CPB at this AU's removal) / BitRate,
 *                   rounded UP: the wait a decoder starting here must take
 *                   before removing this AU, and it may never claim more
 *                   fill than the encoder planned against. At the first AU
 *                   that is exactly --vbv-init * --vbv-bufsize, which is
 *                   where src/encoder/vbv.c starts its own walk.
 *   init_offset     the rest of the buffer-full time, so that
 *                   init_delay + init_offset = 90000 * CpbSize / BitRate.
 *   cpb_delay       AuCpbRemovalDelayVal: clock ticks from the removal of
 *                   the AU that carried the most recent PRECEDING buffering
 *                   period to this AU's. One picture per tick
 *                   (fixed_pic_rate_general_flag is 1), so it is the
 *                   difference of coding indices. On the AU that opens the
 *                   first buffering period of the stream nothing precedes
 *                   it, the value is not used by C.2.3, and it is 1.
 *   dpb_delay       pic_dpb_output_delay: the reorder distance in frames,
 *                   sps_max_num_reorder_pics + POC - (coding index within
 *                   the CVS). An IDR gets the reorder depth itself; the
 *                   B frame coded before the picture it follows in output
 *                   gets more; a picture coded last and output last gets
 *                   the same. It cannot go negative: the reorder bound is
 *                   the definition of how far coding order may run ahead.
 */

#ifndef N265_HRD_H
#define N265_HRD_H

#include <stdint.h>

/* D.2.2 and D.2.3's fields are u(v) of initial_cpb_removal_delay_length and
 * au_cpb_removal_delay_length / dpb_output_delay_length bits. write_hrd
 * declares all three as 24 (the minus1 fields are 23), which is the usual
 * choice and the one this struct's ranges assume. */
#define N265_HRD_DELAY_BITS 24
#define N265_HRD_DELAY_MAX  0xffffffu

typedef struct n265_hrd_sei {
    int bp;                 /* this AU opens a buffering period (IRAP) */
    uint32_t init_delay;    /* initial_cpb_removal_delay[0], 90 kHz ticks */
    uint32_t init_offset;   /* initial_cpb_removal_offset[0]              */
    uint32_t cpb_delay;     /* AuCpbRemovalDelayVal, >= 1 (minus1 is coded) */
    uint32_t dpb_delay;     /* pic_dpb_output_delay                       */
} n265_hrd_sei;

#endif
