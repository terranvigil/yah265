# The fingerprint census: which choices are free, and which of them a reader can see

Phase 14 item 10, step 1. The backlog entry proposed a signature in "the
choices the syntax leaves open (tie-breaks in the merge, motion and intra
searches; the short-term reference set signaling form), zero bits and no
picture change". This is the measurement that says which of those are
actually available, and it moves the design: **an RD tie is not a
fingerprint site.**

## The test a site has to pass

Three conditions, and all three at once.

1. **Free.** The two codings must cost the same number of bits. Not
   "about the same" - the same, or the identity cmp, the rate control and
   the board all read the mark.
2. **Neutral.** The two codings must reconstruct the same picture, and
   must leave the same state behind for everything coded after them.
3. **Provable by a reader.** Somebody holding only the bitstream has to
   be able to tell that a choice was available. This is the condition the
   entry did not state and the one that does the work.

Condition 3 is what rules out the searches. When two merge candidates, two
motion vectors or two intra modes come out with the same RD cost, the
encoder is free to take either - but nothing in the bitstream records
that they tied. A reader would have to re-run the encoder's cost model on
the encoder's own reconstruction to know a choice existed, and then it is
not reading a mark, it is guessing at one. **A tie in the cost is not a
tie in the syntax.**

What passes condition 3 is a *syntax-level equivalence*: two codings the
decoder itself can see are interchangeable, because its own normative
derivation produces the same answer from both.

## The counts

`N265_FP_CENSUS=1`, `--preset medium`, 60 frames, `--threads 1`, at the
CQP pin (`--qp 27 --aq 0 --cutree 0`) and at `--crf 28`. Per frame,
averaged over the clip.

| site | foreman CQP | foreman CRF | park_joy CQP | park_joy CRF | touchdown CQP | touchdown CRF | free? | reader-provable? |
|---|---|---|---|---|---|---|---|---|
| merge PUs committed | 390.1 | 274.1 | 5060.1 | 3644.6 | 5822.9 | 4407.8 | - | - |
| merge picks with an exact RD tie | 129.8 | 99.4 | 3433.3 | 2702.2 | 1923.9 | 1681.5 | yes | **no** |
| merge lists holding any duplicate pair | 199.3 | 164.7 | 2805.4 | 2442.1 | 3522.2 | 3045.8 | see below | yes |
| ...holding one at the LAST TWO indices | 108.6 | 92.0 | 1662.0 | 1629.6 | 1656.0 | 1431.9 | **yes** | **yes** |
| ...and the committed index is one of them | **3.7** | **2.9** | **17.4** | **19.5** | **24.6** | **25.2** | **yes** | **yes** |
| AMVP lists built | 539.1 | 294.2 | 11149.9 | 6269.3 | 5040.2 | 2671.3 | - | - |
| ...whose two candidates are identical | 33.4 | 23.1 | 2213.2 | 1747.3 | 246.0 | 180.5 | **no** | yes |
| subpel squares evaluated | 973.0 | 540.2 | 20211.9 | 11390.9 | 9072.1 | 4865.4 | - | - |
| ...where the winner's cost was matched | 31.9 | 20.5 | 1292.7 | 1125.9 | 210.4 | 132.3 | yes | **no** |
| luma intra mode ranks run | 503.2 | 384.4 | 16439.7 | 13029.1 | 5824.9 | 4906.3 | - | - |
| ...where the best SATD was matched | 41.5 | 33.1 | 1877.1 | 1510.3 | 429.4 | 377.3 | **no** | **no** |
| alternative RPS signaling forms per slice | 0 | 0 | 0 | 0 | 0 | 0 | n/a | yes |

Site by site.

**Merge index, last pair. The one that ships.** `merge_idx` is truncated
unary with cMax `MaxNumMergeCand - 1` = 4, so its five codings are 1, 2,
3, 4 and 4 bins long. Only the last two have the same length, and they
differ in one BYPASS bin: the context-coded prefix is the same value in
both, and a bypass bin costs exactly one bit whichever way it goes and
updates no context. So writing 4 where 3 was chosen is free to the bit
and leaves the CABAC state where it was.

It is neutral because the site requires the two candidates to hold the
same motion, which means the same reconstruction and the same entry in
the motion field every later derivation reads.

It is provable because the decoder builds the merge candidate list
itself - that is its job - and sees the duplicate.

Where the duplicates come from is the zero-candidate fill (8.5.3.2.5):
once the reference index walk passes `numRefIdx` every further zero
candidate repeats the last one, so any list that falls through to the
fill with two slots left ends in a duplicate pair. That is why the
"holding one at the last two indices" row is a quarter to a third of all
merge PUs. What makes the usable row two orders of magnitude smaller is
the other half of the condition: the encoder's own pick has to LAND
there, and it rarely does - a zero candidate is what the list holds when
nothing better was found.

**The budget, then, is 3-5 free bits per frame on CIF and 17-25 on 720p
and 1080p**, measured, not assumed. It is enough: the signature is 32
bits and the scheme is position-keyed, so bits accumulate across a clip
rather than needing a whole signature inside one frame.

**AMVP with identical candidates. Free? No.** 8.5.3.2.6 fills to two
candidates with zero vectors without pruning them against what is already
there, so the two entries really can be identical, 17-2213 times a frame,
and a decoder can see it. But `mvp_l0_flag` / `mvp_l1_flag` is a single
CONTEXT-coded bin: its two values cost different numbers of bits and
leave the context in different states. Excluded on condition 1, and it is
the largest site given up.

**Intra modes. Free? No, and not provable either.** Two things were
measured and both fail. The equal-SATD ties (33-1877 a frame) are a cost
tie, so condition 3 fails the same way the merge ties do. And the
syntax-level version - two modes whose prediction is identical because
every reference sample is equal - fails condition 2: the chosen mode
seeds the MPM derivation of the blocks below and to the right, whose
`mpm_idx` (1-2 bypass bins) against `rem_intra_luma_pred_mode` (5 bypass
bins) is a different bit count; it selects the chroma DM mode; and it
selects the mode-dependent scan of 4x4 and 8x8 intra TBs. A choice that
is free in its own syntax element and not free three blocks later is not
free.

**Subpel and motion-search ties. Provable? No.** Same reason as the merge
cost ties. 20-1300 squares a frame end with two positions at the same
cost, and the bitstream records only the one that was taken.

**The short-term RPS signaling form. It does not exist in our streams.**
`num_short_term_ref_pic_sets` is 0 in the SPS
(`src/encoder/headers.c`), so every slice codes its set explicitly and
there is no index form to choose against - the census column is 0 by
construction, not by measurement. The entry named this site and it is
worth saying exactly what it would cost to create it:

- Putting the sets in the SPS and signaling by index makes
  `short_term_ref_pic_set_idx` a FIXED-LENGTH u(v) field, so choosing
  among duplicate entries in that list would be genuinely free per slice
  and readable by a plain header parser - the best of both conditions.
  But the duplicates cost SPS bits once, and moving the sets out of the
  slice headers changes every slice's size. It is a stream size change,
  in fact an improvement, and therefore not a zero-bit fingerprint. It is
  a compression item, not this one.
- Keeping the explicit form and choosing between equivalent explicit
  codings is not available either: in the slice header `stRpsIdx` equals
  `num_short_term_ref_pic_sets`, which is 0, so 7.3.7 does not even code
  `inter_ref_pic_set_prediction_flag` and there is exactly one legal
  spelling.

## The finding that decides the design

Every field in the slice header as we emit it was walked for a second
legal value of the same length that changes no sample:
`first_slice_segment_in_pic_flag`, `pic_order_cnt_lsb`, the RPS,
`slice_temporal_mvp_enabled_flag`, the SAO flags,
`num_ref_idx_active_override_flag`, `mvd_l1_zero_flag`,
`collocated_from_l0_flag`, `collocated_ref_idx`,
`five_minus_max_num_merge_cand`, `slice_qp_delta`, the deblocking
override. There is exactly one - `slice_temporal_mvp_enabled_flag` on a
non-IDR I slice, which parses no collocated syntax - and it occurs once
per intra picture, which is once per keyint. Everything else either costs
bits or changes decoding.

So:

> **In HEVC, a free mark has to live inside the CABAC-coded slice data,
> and a mark a header parser can read has to pay for its own bits.**

That is not a limitation of this encoder. It is what it means for a
format to be well designed: the header has no slack, and the slack that
exists is in the redundancy of the entropy-coded layer.

The consequence for the verifier is direct and is written up in
docs/stream-fingerprint.md: reading the mark back needs a decoder in the
loop, and `scripts/fingerprint_check.py` drives one.

## Reproducing this

    N265_FP_CENSUS=1 build/n265 tests/corpus/foreman_cif.y4m --frames 60 \
        --preset medium --qp 27 --aq 0 --cutree 0 --threads 1 \
        -o /dev/null 2> census.csv

One `FPC,` row per frame: display index, slice type, QP, then the eleven
counters in the order of `N265_FPC_*` in `src/encoder/encoder.h`. The
counters live on the frame_work and are summed out of the row lanes in
`n265_row_end`, so the census reads the same at any thread count.
