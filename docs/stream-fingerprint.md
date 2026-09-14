# The stream fingerprint

Phase 14 item 10. The second of the two provenance marks in yah264's
docs/stream-provenance.md: the one that survives having the first one
stripped. The measurement it rests on is
docs/stream-fingerprint-census.md, and the census is what moved the
design away from the search tie-breaks the backlog entry proposed.

## What it is

The identification SEI (`--info`, phase 14 item 9) is the readable mark:
a UUID and a line of text naming the encoder, its version, its licence,
its URL and the options the run resolved. It is also the first thing a
remux or a one-line NAL filter drops, and nothing about a stream then
says what wrote it.

The fingerprint is a signature in a choice the syntax leaves open.

> Where the merge candidate list holds the **same motion at both of its
> last two indices**, `merge_idx` may name either one. The two codings
> are the same length, their context-coded prefix is the same value, and
> they differ in a single bypass bin, which costs exactly one bit
> whichever way it goes. They reconstruct the same picture and leave the
> same motion field behind. So which of the two is written is free, and a
> fixed pattern in that choice names the encoder without spending a bit.

`merge_idx` is truncated unary with cMax `MaxNumMergeCand - 1` = 4, so
its five codings are 1, 2, 3, 4 and 4 bins long: the last pair is the
only equal-length pair, which is why the site is that pair and no other.
Duplicates arrive there from the zero-candidate fill of 8.5.3.2.5, which
repeats its last entry once the reference index walk passes `numRefIdx`.

It is on by default. `--no-fingerprint` drops it, and that is the flag an
identity cmp against a binary older than this item is run with.

## The signature

32 bits.

| bits | content |
|---|---|
| 0..23 | the first three bytes of the identification SEI's UUID: `7a a4 16`. Fixed for the life of the format. |
| 24..31 | the encoder version, packed major 3 : minor 3 : patch 2. Moves with the release. |

One table for both marks, which is the point: a reader that knows the SEI
knows the fingerprint. The sibling encoders take the same 24-and-8 layout
with their own UUID's first three bytes, so the three schemes are one
scheme and a verifier written for any of them is a three-line change from
the others.

## Which bit a site carries

Not the next one. **The site's own position decides it:**

    z = (poc_lsb * 0x9e3779b1) ^ (x0 * 0x85ebca6b) ^ (y0 * 0xc2b2ae35)
    z ^= z >> 15;  z *= 0x2545f491;  z ^= z >> 13
    bit = signature[z & 31]

where `poc_lsb` is the `pic_order_cnt_lsb` a reader sees (0 for an IDR,
which signals none) and `(x0, y0)` is the PU's top-left in luma samples.
PU top-left positions are unique within a picture - the coding quadtree
is a partition - so this is well defined, and both sides compute it from
public syntax alone.

Two things fall out of position keying and both are load-bearing.

**The stream does not depend on the schedule.** A running site counter
would have made the payload depend on how many sites came before, which
is fine within a picture and useless across one: the encoder has K frames
in flight. Keyed on the position, the answer is the same at any thread
count, and the determinism gate reads it that way.

**A reader that can only see half the sites still reads the mark.** A
decoder derives only as many merge candidates as the index it read asks
for (`merge_idx + 1`, and the spec asks for
no more), so the duplicate pair is visible when the HIGH index was
written and invisible when the low one was. With a running counter that
would be fatal - the reader could not count. Position keying turns it
into a one-sided test:

> every mark a reader sees says "the signature's bit at this position is
> 1", and the test is that no mark ever lands on a position where the
> signature's bit is 0.

An encoder choosing the high index for its own reasons lands on a zero
bit 13 times in 24, so twenty clean marks is about one in a hundred
thousand, and `scripts/fingerprint_check.py` wants twenty of them and a
covering of the signature's one bits before it will say yes.

## Where it happens in the encoder

`src/encoder/fp.c` holds the signature and the keying.

The site is spotted at commit time (`fp_merge_look` in
`src/encoder/ctu.c`), where the merge candidate list is rebuilt for the
committed PU - and only when the committed index is one of the last two,
so the cost is a list rebuild on a few PUs a frame. The answer rides the
CU record: bit 3 of a skip record, bit 6 of an inter PU record.

The choice is MADE in the entropy pass (`fp_merge_idx` in
`src/encoder/entropy.c`), because that is the pass whose order is the
bitstream's and whose output is what a reader sees. It is gated on
`in_entropy`, so the rate estimator - which replays the same records -
does not take it. It would cost nothing either way, the swap being
bit-count neutral, but the syntax a reader sees should be decided in one
place.

Nothing before the entropy pass knows the fingerprint exists. No mode
decision, no cost, no reconstruction and no rate control reads it, which
is what makes the zero-cost proof below possible at all.

## The verifier

`scripts/fingerprint_check.py <stream>` prints both marks:

    identification: yah265 0.0.1 - H.265/HEVC encoder - Copyright 2026 ...
    fingerprint:    yah265 (HEVC), version 0.0.1

The identification leg it reads on its own - Annex-B framing, emulation
prevention, SEI payload walk, all plain parsing. `--strip-sei` drops
every SEI NAL first, which is what a remux does, and is how the
fingerprint's whole reason for existing gets demonstrated in one command.

The fingerprint leg needs a decoder, and this is not a shortcut - it is
the census's finding. A free mark in HEVC has to live inside the
CABAC-coded slice data, because the slice header has no slack (see
docs/stream-fingerprint-census.md, "The finding that decides the
design"). So the script drives one: libde265's `dec265` built with
`DE265_LOG_TRACE`, which prints every `merge_idx` it decodes and the
candidate list it derived. `--dec265` or `$DEC265_TRACE` names it, and it
defaults to `~/src/libde265/build-trace/dec265/dec265`. Without one the
fingerprint line says it could not read rather than guessing.

The dependency is the same shape as the conformance gate's, which already
holds every encode to `dec265` and `ffmpeg`. Nothing about the MARK
depends on libde265: any HEVC decoder derives the same candidate list,
because deriving it is what decoding a merge PU IS.

## What it costs: nothing, measured

`--preset medium`, 60 frames, `--threads 1`, `--no-info` on both arms so
the identification SEI's options clause (which names `--no-fingerprint`
when it is given) is out of the way.

| clip | mode | bytes with | bytes without | count | recon | bytes that differ |
|---|---|---|---|---|---|---|
| foreman_cif | `--qp 27 --aq 0 --cutree 0` | 67489 | 67489 | same | identical | 49 |
| foreman_cif | `--crf 28` | 29448 | 29448 | same | identical | 45 |
| foreman_cif | `--bitrate 1500` | 373353 | 373353 | same | identical | 61 |
| park_joy_720p | `--qp 27 --aq 0 --cutree 0` | 2931926 | 2931926 | same | identical | 324 |
| park_joy_720p | `--crf 28` | 1043206 | 1043206 | same | identical | 359 |
| park_joy_720p | `--bitrate 1500` | 217185 | 217185 | same | identical | 281 |

**The byte COUNT is identical on all six and the bytes are not**, which
is exactly the claim: the mark changes which bits are written and not how
many. `--dump-recon` is byte-identical on all six, so no sample moved.

The ABR row is the one that matters most. Under `--bitrate` the rate
control spends every bit the stream carries, so a mark that cost anything
would show up there as a different size, a different QP trajectory and a
different picture. It does not.

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
| ASan + UBSan on the fast conformance list | see docs/phase14.md |

## The sites that were measured and refused

Short version; the numbers and the reasoning are in the census.

- **RD ties in the merge, motion and intra searches** - the entry's own
  first proposal, and 100-3400 of them a frame. A tie in the COST is not
  a tie in the SYNTAX: nothing in the bitstream records that two
  candidates came out equal, so a reader would have to re-run the
  encoder's cost model to know a choice was there. Refused on
  readability, not on cost.
- **AMVP with two identical candidates** - 17-2213 a frame and perfectly
  visible to a decoder, but `mvp_lX_flag` is a context-coded bin, so its
  two values cost different numbers of bits. The largest site given up.
- **Intra modes with identical predictions** - free in their own syntax
  element and not free three blocks later: the mode seeds the MPM
  derivation of its neighbours, the chroma DM mode and the mode-dependent
  scan.
- **The short-term RPS signalling form** - named in the entry, and it
  does not exist in our streams: `num_short_term_ref_pic_sets` is 0, so
  every slice codes its set explicitly and 7.3.7 offers exactly one
  spelling. Creating the choice means moving the sets into the SPS, which
  changes every slice's size. That is a compression item, not a
  fingerprint.
