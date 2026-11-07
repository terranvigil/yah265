# The stream fingerprint

This is phase 14 item 10. yah264's docs/stream-provenance.md describes two
provenance marks, and this is the second of them. It survives having the
first one stripped. The measurement it rests on is
docs/stream-fingerprint-census.md. The census is what moved the design away
from the search tie-breaks the backlog entry proposed.

## What it is

The identification SEI is the readable mark. It's `--info`, from phase 14
item 9. It holds a UUID and a line of text naming the encoder, its version,
its license, its URL and the options the run resolved. It's also the first
thing a remux or a one-line NAL filter drops. After that, nothing about the
stream says what wrote it.

The fingerprint is a signature in a choice the syntax leaves open.

> Where the merge candidate list holds the same motion at both of its last
> two indices, `merge_idx` may name either one. The two codings are the
> same length. Their context-coded prefix is the same value. They differ in
> a single bypass bin, and that bin costs exactly one bit whichever way it
> goes. They reconstruct the same picture and leave the same motion field
> behind. Which of the two is written is therefore free. A fixed pattern in
> that choice names the encoder without spending a bit.

`merge_idx` is truncated unary with cMax `MaxNumMergeCand - 1` = 4. Its five
codings are 1, 2, 3, 4 and 4 bins long. The last pair is the only
equal-length pair, which is why the site is that pair and no other.
Duplicates arrive there from the zero-candidate fill of 8.5.3.2.5. That fill
repeats its last entry once the reference index walk passes `numRefIdx`.

It's on by default. `--no-fingerprint` drops it. An identity cmp against a
binary older than this item is run with that flag.

## The signature

32 bits.

| bits | content |
|---|---|
| 0..23 | the first three bytes of the identification SEI's UUID: `7a a4 16`. Fixed for the life of the format. |
| 24..31 | the encoder version, packed major 3 : minor 3 : patch 2. Moves with the release. |

With one table for both marks, a reader that knows the SEI knows the
fingerprint. That's the point. The sibling encoders take the same 24-and-8 layout
with their own UUID's first three bytes. That makes the three schemes one
scheme. A verifier written for any of them is a three-line change from the
others.

## Which bit a site holds

It isn't simply the next bit of the signature. The site's own position
decides it:

    z = (poc_lsb * 0x9e3779b1) ^ (x0 * 0x85ebca6b) ^ (y0 * 0xc2b2ae35)
    z ^= z >> 15;  z *= 0x2545f491;  z ^= z >> 13
    bit = signature[z & 31]

Here `poc_lsb` is the `pic_order_cnt_lsb` a reader sees. It's 0 for an IDR,
which signals none. `(x0, y0)` is the PU's top-left in luma samples. PU
top-left positions are unique within a picture because the coding quadtree
is a partition. That makes the hash well defined. Both sides compute it from
public syntax alone.

Position keying gives two properties, and the design depends on both.

The stream doesn't depend on the schedule. A running site counter would have
made the payload depend on how many sites came before. That works within a
picture and fails across pictures, because the encoder has K frames in
flight. Keyed on the position, the answer is the same at any thread count.
The determinism gate reads it that way.

A reader that can only see half the sites still reads the mark. A decoder
derives only as many merge candidates as the index it read asks for:
`merge_idx + 1`, and the spec asks for no more. The duplicate pair is visible when the HIGH index was written. It's
invisible when the low one was. With a running counter that would be fatal
because the reader couldn't count. Position keying turns it into a one-sided
test:

> every mark a reader sees says "the signature's bit at this position is
> 1". The test is that no mark ever falls on a position where the
> signature's bit is 0.

An encoder choosing the high index for its own reasons hits a zero bit 13
times in 24. For such an encoder, twenty clean marks is about one in a hundred thousand.
`scripts/fingerprint_check.py` wants twenty of them before it will say yes.
It also wants them to cover the signature's one bits.

## Where it happens in the encoder

`src/encoder/fp.c` holds the signature and the keying.

The site is spotted at commit time by `fp_merge_look` in
`src/encoder/ctu.c`. There the merge candidate list is rebuilt for the
committed PU. That happens only when the committed index is one of the last
two, so the cost is a list rebuild on a few PUs a frame. The answer rides
the CU record. It's bit 3 of a skip record and bit 6 of an inter PU record.

The choice is MADE in the entropy pass, by `fp_merge_idx` in
`src/encoder/entropy.c`. That pass writes in the bitstream's order. Its
output is what a reader sees. The choice runs only under `in_entropy`.
The rate estimator replays the same records and doesn't take it. It would
cost nothing either way because the swap is bit-count neutral. The syntax a
reader sees should still be decided in one place.

Nothing before the entropy pass knows the fingerprint exists. No mode
decision, no cost, no reconstruction and no rate control reads it. That's
what makes the zero-cost proof below possible at all.

## The verifier

`scripts/fingerprint_check.py <stream>` prints both marks:

    identification: yah265 0.0.1 - H.265/HEVC encoder - Copyright 2026 ...
    fingerprint:    yah265 (HEVC), version 0.0.1

It reads the identification leg on its own. That's plain parsing of the
Annex-B framing, the emulation prevention and the SEI payload walk.
`--strip-sei` drops every SEI NAL first, as a remux does. One command with
it shows the fingerprint's whole reason for existing.

The fingerprint leg needs a decoder. It's no shortcut: the census found that
it must. A free mark in HEVC has to live inside the CABAC-coded
slice data, because the slice header has no slack. The census covers this
in docs/stream-fingerprint-census.md, under "The finding that decides the
design". So the script drives a decoder. It's libde265's `dec265` built with
`DE265_LOG_TRACE`, which prints every `merge_idx` it decodes and the
candidate list it derived. `--dec265` or `$DEC265_TRACE` names it. The
default is `~/src/libde265/build-trace/dec265/dec265`. Without one the
fingerprint line says it couldn't read the mark. It doesn't guess.

The dependency has the same form as the conformance gate's, which already
holds every encode to `dec265` and `ffmpeg`. Nothing about the MARK depends
on libde265. Any HEVC decoder derives the same candidate list, because
deriving it is what decoding a merge PU IS.

Full tracing prints a line per syntax element. A 720p clip is tens of
millions of lines and minutes of wall. The mark accumulates fast enough that
this doesn't matter. 60 frames of CIF gives 30 marks against a threshold of
20. 60 frames of CIF Main10 gives 26. On HD, pass `--frames`.

## What the fingerprint costs

It costs nothing, as measured. Both arms run `--preset medium`, 60 frames
and `--threads 1`. Both take
`--no-info` so the identification SEI's options clause is out of the way.
That clause names `--no-fingerprint` when it's given.

| clip | mode | bytes with | bytes without | count | recon | bytes that differ |
|---|---|---|---|---|---|---|
| foreman_cif | `--qp 27 --aq 0 --cutree 0` | 67489 | 67489 | same | identical | 49 |
| foreman_cif | `--crf 28` | 29448 | 29448 | same | identical | 45 |
| foreman_cif | `--bitrate 1500` | 373353 | 373353 | same | identical | 61 |
| park_joy_720p | `--qp 27 --aq 0 --cutree 0` | 2931926 | 2931926 | same | identical | 324 |
| park_joy_720p | `--crf 28` | 1043206 | 1043206 | same | identical | 359 |
| park_joy_720p | `--bitrate 1500` | 217185 | 217185 | same | identical | 281 |

The byte COUNT is identical on all six, and the bytes themselves differ.
That's exactly the claim. The mark changes which bits are written and not
how many. `--dump-recon` is byte-identical on all six, so no sample moved.

The ABR row matters most. Under `--bitrate` the rate control spends every
bit the stream holds. A mark that cost anything would show up there as a
different size, a different QP trajectory and a different picture. It
doesn't.

## How to tell which arm you're holding

`--no-fingerprint` is a passthrough. It writes the index the encoder's own
mode decision committed, whatever that was. That's a stronger guarantee than
"always the lower index" would be. It's why the escape reproduces a pre-item
binary byte for byte and not approximately.

This is against a fresh build of `64f267b`, the commit before the item. It
ran 60 frames at `--preset medium`, with `--no-info` on both arms and AQ and
cu-tree at their defaults:

| clip | mode | 64f267b | `--no-fingerprint` | default | escape | recon | bytes the MARK moves |
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

Did the pre-item encoder ever write the HIGH index at a duplicate pair? On
this evidence it didn't. `64f267b`'s own foreman streams write the last
index 703 (CQP), 555 (CRF) and 1414 (ABR) times in 60 frames.
`scripts/fingerprint_check.py` reads **0 marks** in all three. Not one of
those is a position where the last two candidates duplicate. The cost model
says that's what it should do. It prices `merge_idx` MAX_MERGE-2 and
MAX_MERGE-1 equally
(`b += 256u * (midx < MAX_MERGE - 1 ? midx : midx - 1)`), and the first
minimum wins. The two possible escape semantics coincide here. The
passthrough is the one that's true by construction instead of by
measurement.

If an escape cmp against an older binary fails, check which arm you're
actually holding before looking anywhere else:

    python3 scripts/fingerprint_check.py <stream>

A stream written with `--no-fingerprint` says `no fingerprint`. One written
without it says `yah265 (HEVC)`. The two arms have the same byte count and
the same recon. By design, `ls -l` and `--dump-recon` can't tell them apart.
A cmp of the ON arm against a pre-item binary looks exactly like a broken
escape. On foreman_cif `--qp 27 --frames 60 --no-info` the ON arm first
differs from `64f267b` at **byte 12914 of 64529**. It moves 54 bytes in all.

There are two more places the flag doesn't reach. A binary older than the
item REJECTS `--no-fingerprint` with the usage text and writes nothing, so a
stale build can't quietly produce a marked stream. The other is a program
that skips the CLI, because the switch is a CLI flag that reaches a
process-global setter through the API table. A harness that drives
the library directly, and not through `cli/n265.c`, never calls
`n265_set_fingerprint`. It gets the default, which is on.

## The gate

| check | result |
|---|---|
| `make test` (15 suites incl. the new `fp` unit test) | pass |
| `make conformance-fast`, both decoders, fingerprint ON | **122 pass, 0 fail** |
| `make conformance10-fast` (Main10), fingerprint ON | see docs/phase14.md |
| the 6-cell zero-cost table above | 6/6 count-identical, recon-identical |
| determinism `THREADS="1 12"`, t12 repeated, 8 spinners, foreman_cif | **108/108 cells reproducible** |
| the verifier on our stream | reads it |
| the verifier with every SEI stripped | reads it, and the identification line goes to `none` |
| the verifier on `--no-fingerprint` | `no fingerprint`, 0 marks out of 745 merge PUs at the last index |
| the verifier on x265 4.3's stream | `no fingerprint`, 0 marks out of 2222 |
| the verifier on a Main10 stream | reads it: 26 marks, foreman_cif `--crf 28 --output-depth 10`, 60 frames |
| ASan + UBSan on the fast conformance list | see docs/phase14.md |

## The sites that were measured and refused

This is the short version. The census has the numbers and the reasoning.

- RD ties in the merge, motion and intra searches were the entry's own first
  proposal. There are 100-3400 of them a frame. A tie in the COST isn't a tie
  in the SYNTAX. Nothing in the bitstream records that two candidates came
  out equal. A reader would have to re-run the encoder's cost model to know a
  choice was there. They were refused on readability, not on cost.
- AMVP with two identical candidates happens 17-2213 times a frame and is
  perfectly visible to a decoder. But `mvp_lX_flag` is a context-coded bin,
  so its two values cost different numbers of bits. It's the largest site
  given up.
- Intra modes with identical predictions are free in their own syntax
  element. They aren't free three blocks later. The mode seeds the MPM
  derivation of its neighbors, the chroma DM mode and the mode-dependent
  scan.
- The short-term RPS signaling form was named in the entry. It doesn't
  exist in our streams. Because `num_short_term_ref_pic_sets` is 0, every
  slice codes its set explicitly. Then 7.3.7 offers exactly one spelling. Creating
  the choice means moving the sets into the SPS. That changes every slice's
  size. That would be a compression item and not a fingerprint.
