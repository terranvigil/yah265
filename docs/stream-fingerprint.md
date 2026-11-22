# The stream fingerprint

This is phase 14 item 10. It is the second of the two provenance marks that
yah264's docs/stream-provenance.md describes. It survives when the first one
is stripped. The design rests on the
[fingerprint census](stream-fingerprint-census.md). The census moved the
design away from the search tie-breaks I first proposed.

## The mark

The identification SEI is the readable mark, written under `--info`. It
holds a UUID and a line of text naming the encoder, its version, its license,
its URL and the options the run resolved. It's also the first thing a remux
or a one-line NAL filter drops. After that, nothing about the stream says
what wrote it.

The fingerprint is a signature in a choice the syntax leaves open.

> Where the merge candidate list holds the same motion at both of its last
> two indices, `merge_idx` may name either one. The two codings are the
> same length. Their context-coded prefix is the same value. They differ in
> a single bypass bin, which costs exactly one bit at either value. They
> reconstruct the same picture and leave the same motion field behind. Which
> of the two is written is therefore free. A fixed pattern in that choice
> names the encoder without spending a bit.

`merge_idx` is truncated unary with cMax `MaxNumMergeCand - 1` = 4. Its five
codings are 1, 2, 3, 4 and 4 bins long. The site is the last pair because
it's the only pair of equal length. Duplicates arrive there from the
zero-candidate fill of 8.5.3.2.5. That fill repeats its last entry once the
reference index walk passes `numRefIdx`.

It's on by default. `--no-fingerprint` drops it. Run an identity cmp against
a binary older than this item with that flag.

## The signature

The signature is 32 bits.

| bits | content |
|---|---|
| 0..23 | the first three bytes of the identification SEI's UUID: `7a a4 16`. Fixed for the life of the format. |
| 24..31 | the encoder version, packed major 3 : minor 3 : patch 2. Moves with the release. |

Because one table serves both marks, a reader that knows the SEI knows the
fingerprint. The sibling encoders take the same 24-and-8 layout with their
own UUID's first three bytes. A verifier written for one of them is a
three-line change from the others.

## Which bit a site holds

The site's own position decides which bit of the signature it holds:

    z = (poc_lsb * 0x9e3779b1) ^ (x0 * 0x85ebca6b) ^ (y0 * 0xc2b2ae35)
    z ^= z >> 15;  z *= 0x2545f491;  z ^= z >> 13
    bit = signature[z & 31]

Here `poc_lsb` is the `pic_order_cnt_lsb` a reader sees. It's 0 for an IDR,
which signals none. `(x0, y0)` is the PU's top-left in luma samples. Because
the coding quadtree is a partition, PU top-left positions are unique within a
picture and the hash is well defined. Both sides compute it from public
syntax alone.

Keying on position keeps the stream independent of the schedule. A running
site counter would have made the payload depend on how many sites came
before. That works within a picture and fails across pictures, because the
encoder has K frames in flight. Keyed on the position, the answer is the
same at any thread count.

Position keying also lets a reader that sees only half the sites read the
mark. A decoder derives only as many merge candidates as the index it read
asks for, `merge_idx + 1`. The duplicate pair is visible when the high index
was written and invisible when the low one was. A reader couldn't count
sites for a running counter. With position keying the test is one-sided:

> Every mark a reader sees says "the signature's bit at this position is
> 1". The test is that no mark ever falls on a position where the
> signature's bit is 0.

An encoder choosing the high index for its own reasons hits a zero bit 13
times in 24. For such an encoder, twenty clean marks is about one in a
hundred thousand. The [verifier](../scripts/fingerprint_check.py) wants
twenty of them before it will say yes. It also wants them to cover the
signature's one bits.

## Where it happens in the encoder

`src/encoder/fp.c` holds the signature and the keying.

`fp_merge_look` in `src/encoder/ctu.c` spots the site at commit time. It
rebuilds the merge candidate list for the committed PU only when the
committed index is one of the last two. The cost is a list rebuild on a few
PUs a frame. The answer is stored in the CU record, in bit 3 of a skip
record and bit 6 of an inter PU record.

`fp_merge_idx` in `src/encoder/entropy.c` makes the choice in the entropy
pass. That pass writes in the bitstream's order. Its output is what a
reader sees. The choice runs only under `in_entropy`. The rate estimator
replays the same records and doesn't take it. Because the swap is bit-count
neutral, taking it there would cost nothing. I still want the syntax a
reader sees decided in one place.

Nothing before the entropy pass knows the fingerprint exists. No mode
decision, cost, reconstruction or rate control reads it. The zero-cost
measurement below depends on that.

## The verifier

`scripts/fingerprint_check.py <stream>` prints both marks:

    identification: yah265 0.0.1 - H.265/HEVC encoder - Copyright 2026 ...
    fingerprint:    yah265 (HEVC), version 0.0.1

The script reads the identification SEI itself, by parsing the Annex-B
framing, the emulation prevention and the SEI payload. With `--strip-sei`
it drops every SEI NAL first, as a remux does, and still reads the
fingerprint.

Reading the fingerprint needs a decoder. A free mark in HEVC has to be
inside the CABAC-coded slice data, because the slice header has no slack.
The [census](stream-fingerprint-census.md) covers this under "The finding
that decides the design". The script drives libde265's `dec265` built with
`DE265_LOG_TRACE`, which prints each `merge_idx` it decodes and the
candidate list it derived. `--dec265` or `$DEC265_TRACE` names the binary.
The default is `~/src/libde265/build-trace/dec265/dec265`. Without a decoder
the fingerprint line says it couldn't read the mark.

The conformance gate has the same dependency: it already holds each encode
to `dec265` and `ffmpeg`. The mark itself doesn't depend on libde265. Any
HEVC decoder derives the same candidate list, because decoding a merge PU
means deriving it.

Full tracing prints a line per syntax element. A 720p clip is tens of
millions of lines and minutes of wall time. A short run is enough to read
the mark. 60 frames of CIF gives 30 marks against a threshold of 20. 60
frames of CIF Main10 gives 26. On HD, pass `--frames`.

## What the fingerprint costs

The fingerprint costs nothing in the six cells below. Both arms run
`--preset medium`, 60 frames and `--threads 1`. Both take `--no-info`,
because the identification SEI's options clause names `--no-fingerprint`
when it's given.

| clip | mode | bytes with | bytes without | count | recon | bytes that differ |
|---|---|---|---|---|---|---|
| foreman_cif | `--qp 27 --aq 0 --cutree 0` | 67489 | 67489 | same | identical | 49 |
| foreman_cif | `--crf 28` | 29448 | 29448 | same | identical | 45 |
| foreman_cif | `--bitrate 1500` | 373353 | 373353 | same | identical | 61 |
| park_joy_720p | `--qp 27 --aq 0 --cutree 0` | 2931926 | 2931926 | same | identical | 324 |
| park_joy_720p | `--crf 28` | 1043206 | 1043206 | same | identical | 359 |
| park_joy_720p | `--bitrate 1500` | 217185 | 217185 | same | identical | 281 |

The byte count is identical on all six while the bytes themselves differ.
The mark changes which bits are written and not how many. No sample moved:
`--dump-recon` is byte-identical on all six.

The ABR rows matter most. Under `--bitrate` the rate control spends every
bit the stream holds. A mark that cost anything would show up there as a
different size, a different QP trajectory and a different picture.

## The escape

`--no-fingerprint` is a passthrough. It writes the index the encoder's own
mode decision committed. That's a stronger guarantee than "always the lower
index" would be. It's why the escape reproduces a pre-item binary byte for
byte.

The table compares against a fresh build of the commit before the item. Each
cell ran 60 frames at `--preset medium`, with `--no-info` on both arms and AQ
and cu-tree at their defaults:

| clip | mode | pre-item | `--no-fingerprint` | default | escape | recon | bytes the mark moves |
|---|---|---|---|---|---|---|---|
| foreman_cif | `--qp 27` | 64529 | 64529 | 64529 | identical | identical | 54 |
| foreman_cif | `--crf 28` | 29448 | 29448 | 29448 | identical | identical | 45 |
| foreman_cif | `--bitrate 1500` | 373353 | 373353 | 373353 | identical | identical | 61 |
| park_joy_720p | `--qp 27` | 2531215 | 2531215 | 2531215 | identical | identical | 331 |
| park_joy_720p | `--crf 28` | 1043206 | 1043206 | 1043206 | identical | identical | 359 |
| park_joy_720p | `--bitrate 1500` | 217185 | 217185 | 217185 | identical | identical | 281 |

The escape is identical again in these runs:

- at `--preset fast`, at `slow` and at the compiled-in default
- at `--threads` 1, 12 and auto
- with `--output-depth 10`
- with `--ctu 64 --ref 3 --bframes 7`
- under `--lossless`
- with the flag written before the input file instead of after it

On this evidence the pre-item encoder never wrote the high index at a
duplicate pair. In 60 frames its foreman streams write the last index 703
times under CQP, 555 under CRF and 1414 under ABR. The verifier reads 0
marks in all three. Not one of those is a position where the last two
candidates duplicate. The cost model agrees. It prices `merge_idx`
MAX_MERGE-2 and MAX_MERGE-1 equally, and the first minimum wins:

```c
b += 256u * (midx < MAX_MERGE - 1 ? midx : midx - 1)
```

The two possible escape semantics coincide here. I chose the passthrough
because it's true by construction.

If an escape cmp against an older binary fails, first check which arm you're
holding:

    python3 scripts/fingerprint_check.py <stream>

A stream written with `--no-fingerprint` says `no fingerprint`. One written
without it says `yah265 (HEVC)`. Because the two arms have the same byte
count and the same recon, `ls -l` and `--dump-recon` can't tell them apart.
A cmp of the marked arm against a pre-item binary looks like a broken
escape. On foreman_cif at `--qp 27 --frames 60 --no-info` the marked arm
first differs from the pre-item stream at byte 12914 of 64529. It moves 54
bytes in all.

The flag doesn't reach two other places. One is a binary older than the
item, which rejects `--no-fingerprint` with the usage text and writes
nothing. The other is a program that skips the CLI. Because the switch is a
CLI flag that reaches a process-global setter through the API table, a test
program that drives the library directly never calls
`n265_set_fingerprint`. Such a program gets the default, which is on.

## The gate

| check | result |
|---|---|
| `make test`, 15 suites including the new `fp` unit test | pass |
| `make conformance-fast`, both decoders, fingerprint on | 122 pass, 0 fail |
| the 6-cell zero-cost table above | 6/6 count-identical, recon-identical |
| determinism `THREADS="1 12"`, t12 repeated, 8 spinners, foreman_cif | 108/108 cells reproducible |
| the verifier on a yah265 stream | reads it |
| the verifier with every SEI stripped | reads it. The identification line goes to `none` |
| the verifier on `--no-fingerprint` | `no fingerprint`, 0 marks out of 745 merge PUs at the last index |
| the verifier on x265 4.3's stream | `no fingerprint`, 0 marks out of 2222 |
| the verifier on a Main10 stream | reads it: 26 marks, foreman_cif `--crf 28 --output-depth 10`, 60 frames |

## Sites I measured and refused

The [census](stream-fingerprint-census.md) has the numbers and the
reasoning.

- RD ties in the merge, motion and intra searches were my first proposal.
  There are 100-3400 of them a frame. A tie in the cost isn't a tie in the
  syntax. Nothing in the bitstream records that two candidates came out
  equal. A reader would have to re-run the encoder's cost model to know a
  choice was there. I refused them on readability.
- AMVP with two identical candidates happens 17-2213 times a frame, where a
  decoder can see it. Because `mvp_lX_flag` is a context-coded bin, its two
  values cost different numbers of bits. It's the largest site I gave up.
- Intra modes with identical predictions are free in their own syntax
  element. They aren't free three blocks later, because the mode seeds the
  MPM derivation of its neighbors, the chroma DM mode and the mode-dependent
  scan.
- The short-term RPS signaling form was also proposed. That choice doesn't
  exist in yah265's streams. Because `num_short_term_ref_pic_sets` is 0,
  each slice codes its set explicitly, for which 7.3.7 offers exactly one
  spelling. Creating the choice means moving the sets into the SPS. That
  changes every slice's size and would be a compression item.
