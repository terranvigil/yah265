# The fingerprint census

This is step 1 of phase 14 item 10. I first proposed a signature in "the
choices the syntax leaves open (tie-breaks in the merge, motion and intra
searches; the short-term reference set signaling form), zero bits and no
picture change". The census measures which of those choices are available.
It moved the design, because an RD tie turned out not to be a fingerprint
site.

## The test a site has to pass

A site has to pass three conditions at once.

1. Free. The two codings must cost exactly the same number of bits.
   Otherwise the identity cmp, the rate control and the board all read the
   mark.
2. Neutral. The two codings must reconstruct the same picture. They must
   also leave the same state behind for everything coded after them.
3. Provable by a reader. Somebody holding only the bitstream has to be able
   to tell that a choice was available. My proposal didn't state this
   condition.

Condition 3 rules out the searches. When two merge candidates, two motion
vectors or two intra modes come out with the same RD cost, the encoder is
free to take either. Nothing in the bitstream records that they tied. A
reader would have to re-run the encoder's cost model on the encoder's own
reconstruction to know a choice existed. A tie in the cost isn't a tie in
the syntax.

A syntax-level equivalence passes condition 3. That is a pair of codings the
decoder itself can see are interchangeable, because its own normative
derivation produces the same answer from both.

## The counts

The census ran `N265_FP_CENSUS=1` at `--preset medium`, 60 frames and
`--threads 1`. It ran at the CQP pin, `--qp 27 --aq 0 --cutree 0`, and at
`--crf 28`. The counts are per frame, averaged over the clip.

| site | foreman CQP | foreman CRF | park_joy CQP | park_joy CRF | touchdown CQP | touchdown CRF | free? | reader-provable? |
|---|---|---|---|---|---|---|---|---|
| merge PUs committed | 390.1 | 274.1 | 5060.1 | 3644.6 | 5822.9 | 4407.8 | | |
| merge picks with an exact RD tie | 129.8 | 99.4 | 3433.3 | 2702.2 | 1923.9 | 1681.5 | yes | no |
| merge lists holding any duplicate pair | 199.3 | 164.7 | 2805.4 | 2442.1 | 3522.2 | 3045.8 | see below | yes |
| ...holding one at the last two indices | 108.6 | 92.0 | 1662.0 | 1629.6 | 1656.0 | 1431.9 | yes | yes |
| ...and the committed index is one of them | 3.7 | 2.9 | 17.4 | 19.5 | 24.6 | 25.2 | yes | yes |
| AMVP lists built | 539.1 | 294.2 | 11149.9 | 6269.3 | 5040.2 | 2671.3 | | |
| ...whose two candidates are identical | 33.4 | 23.1 | 2213.2 | 1747.3 | 246.0 | 180.5 | no | yes |
| subpel squares evaluated | 973.0 | 540.2 | 20211.9 | 11390.9 | 9072.1 | 4865.4 | | |
| ...where the winner's cost was matched | 31.9 | 20.5 | 1292.7 | 1125.9 | 210.4 | 132.3 | yes | no |
| luma intra mode ranks run | 503.2 | 384.4 | 16439.7 | 13029.1 | 5824.9 | 4906.3 | | |
| ...where the best SATD was matched | 41.5 | 33.1 | 1877.1 | 1510.3 | 429.4 | 377.3 | no | no |
| alternative RPS signaling forms per slice | 0 | 0 | 0 | 0 | 0 | 0 | n/a | yes |

### The last pair of the merge index ships

`merge_idx` is truncated unary with cMax `MaxNumMergeCand - 1` = 4. Its five
codings are 1, 2, 3, 4 and 4 bins long. Only the last two have the same
length. They share the context-coded prefix and differ in one bypass bin. A
bypass bin costs exactly one bit at either value and
updates no context. Writing 4 where 3 was chosen is free to the bit and
leaves the CABAC state where it was.

The site is neutral because it requires the two candidates to hold the same
motion. That means the same reconstruction and the same entry in the motion
field that later derivations read.

It is provable because the decoder builds the merge candidate list itself
and sees the duplicate.

The duplicates come from the zero-candidate fill of 8.5.3.2.5. After the
reference index walk passes `numRefIdx`, the fill repeats its last zero
candidate. Any list that falls through to the fill with two slots left
ends in a duplicate pair. That's why the "holding one at the last two
indices" row counts a quarter to a third of the merge PUs. The usable row is two
orders of magnitude smaller because the encoder's own pick also has to be
one of the pair. It rarely is, because a zero candidate is what the list
holds when nothing better was found.

The measured budget is 3-5 free bits per frame on CIF and 17-25 on 720p and
1080p. That's enough. The signature is 32 bits and the scheme is keyed on
position, which lets bits accumulate across a clip.

### AMVP with identical candidates isn't free

8.5.3.2.6 fills to two candidates with zero vectors and doesn't prune them
against what is already there. The two entries can be identical, 17-2213
times a frame, where a decoder can see it. But `mvp_l0_flag` or
`mvp_l1_flag` is a single context-coded bin. Its two values cost different
numbers of bits and leave the context in different states. The site fails
condition 1. It is the largest site I gave up.

### Intra modes are neither free nor provable

I measured two versions. The equal-SATD ties, 33-1877 a frame, are a cost
tie. They fail condition 3 the way the merge ties do.

The syntax-level version is two modes whose prediction is identical because
all the reference samples are equal. It fails condition 2, because the
chosen mode has three later effects:

- It seeds the MPM derivation of the blocks below and to the right. There
  `mpm_idx` takes 1-2 bypass bins and `rem_intra_luma_pred_mode` takes 5,
  which is a different bit count.
- It selects the chroma DM mode.
- It selects the mode-dependent scan of 4x4 and 8x8 intra TBs.

### Subpel and motion-search ties aren't provable

These fail for the same reason as the merge cost ties. 20-1300 squares a
frame end with two positions at the same cost. The bitstream records only
the one that was taken.

### The short-term RPS signaling form doesn't exist in yah265's streams

`num_short_term_ref_pic_sets` is 0 in the SPS, which
`src/encoder/headers.c` writes. Each slice codes its set explicitly. There's
no index form to choose against. The census column is 0 by
construction. Because I proposed this site, here is what creating it would
cost:

- Putting the sets in the SPS and signaling by index makes
  `short_term_ref_pic_set_idx` a fixed-length u(v) field. Choosing among
  duplicate entries in that list would be free per slice and readable by a
  plain header parser. But the duplicates cost SPS bits once. Moving the
  sets out of the slice headers also changes every slice's size. That change
  improves the stream size. It is a compression item.
- Keeping the explicit form and choosing between equivalent explicit
  codings isn't available either. In the slice header `stRpsIdx` equals
  `num_short_term_ref_pic_sets`, which is 0. Because 7.3.7 then doesn't code
  `inter_ref_pic_set_prediction_flag`, there is exactly one legal spelling.

## The finding that decides the design

I walked the fields of the slice header as yah265 emits it, looking for a
second legal value of the same length that changes no sample:

- `first_slice_segment_in_pic_flag`
- `pic_order_cnt_lsb`
- the RPS
- `slice_temporal_mvp_enabled_flag`
- the SAO flags
- `num_ref_idx_active_override_flag`
- `mvd_l1_zero_flag`
- `collocated_from_l0_flag`
- `collocated_ref_idx`
- `five_minus_max_num_merge_cand`
- `slice_qp_delta`
- the deblocking override

There is exactly one: `slice_temporal_mvp_enabled_flag` on a non-IDR I
slice, which parses no collocated syntax. It occurs once per intra picture,
which is once per keyint. Everything else either costs bits or changes
decoding.

> In HEVC, a free mark has to live inside the CABAC-coded slice data, and a
> mark a header parser can read has to pay for its own bits.

The header has no slack. The slack that exists is in the redundancy of the
entropy-coded layer.

For the verifier, this means reading the mark back needs a decoder in the
loop. The [verifier](../scripts/fingerprint_check.py) drives one, as the
[design note](stream-fingerprint.md) describes.

## Reproducing the census

    N265_FP_CENSUS=1 build/y265 tests/corpus/foreman_cif.y4m --frames 60 \
        --preset medium --qp 27 --aq 0 --cutree 0 --threads 1 \
        -o /dev/null 2> census.csv

The run writes one `FPC,` row per frame: display index, slice type, QP, then
the eleven counters in the order of `N265_FPC_*` in
`src/encoder/encoder.h`. The counters are kept on the frame_work and summed
out of the row lanes in `n265_row_end`. The census reads the same at any
thread count.
