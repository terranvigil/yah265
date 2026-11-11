unit_bitstream = executable('unit_bitstream', 'unit/bitstream_test.c',
  dependencies : yah265_dep)
test('bitstream', unit_bitstream)

# The Annex A level table, checked at every level's own bound and one
# sample/frame/bit over it. It is the exhaustive half of the item 7 table
# check; scripts/level_check.sh is the half that actually encodes.
unit_level = executable('unit_level', 'unit/level_test.c',
  dependencies : yah265_dep)
test('level', unit_level)

# The film grain SEI writer (phase 12 item 3), against a hand-written
# parser of the same syntax table. Pure syntax, no encode.
unit_fgc = executable('unit_fgc', 'unit/fgc_test.c',
  dependencies : yah265_dep)
test('fgc', unit_fgc)

# The stream fingerprint's signature and site keying (phase 14 item 10),
# against the identification SEI's UUID spelled out by hand and against
# the verifier's own arithmetic written a second time. A fingerprint that
# no longer matches the SEI names two different encoders.
unit_fp = executable('unit_fp', 'unit/fp_test.c',
  dependencies : yah265_dep)
test('fp', unit_fp)

# The coefficient-domain noise reduction estimator (phase 14 item 6),
# against a hand-written float reference of the same model. The encoder's
# use of it is checked by a conformance vector and by the escape cmp;
# nothing downstream prints the offset, so the ARITHMETIC has no oracle
# anywhere else.
unit_nr = executable('unit_nr', 'unit/nr_test.c',
  dependencies : yah265_dep)
test('nr', unit_nr)
# The CPB fill arithmetic (src/encoder/vbv.c), checked as algebra: the walk
# of C.2.2, the allowance being exactly the bound that keeps the fill
# non-negative, and the window's cumulative reservation. The encode-side half
# is scripts/vbv_check.py, which re-derives the same trajectory from the
# coded picture sizes.
unit_vbv = executable('unit_vbv', 'unit/vbv_test.c',
  dependencies : yah265_dep)
test('vbv', unit_vbv)

# The two-pass stats parser and the whole-file rate-factor solve
# (src/encoder/twopass.c), checked as algebra for the same reason the VBV
# is: the plan decides every frame's quantizer before a frame is coded, so
# there is nothing in it an encode could tell you that arithmetic cannot.
unit_twopass = executable('unit_twopass', 'unit/twopass_test.c',
  dependencies : yah265_dep)
test('twopass', unit_twopass)
# The HRD timing SEIs (phase 12 item 5): the buffering_period and
# pic_timing writer against a hand-computed message and a hand-written
# parser of the same syntax table. Pure syntax, no encode; the CPB walk
# behind the values is scripts/vbv_check.py's half.
unit_hrd = executable('unit_hrd', 'unit/hrd_test.c',
  dependencies : yah265_dep)
test('hrd', unit_hrd)

# The decoded picture hash (phase 14 item 1): RFC 1321's own vectors
# against the MD5 primitive, a known plane against three expected values
# none of which this code produced, and the three signaling SEI writers
# against hand-assembled messages. Pure arithmetic and pure syntax, no
# encode; the decoder-side half is docs/phase14.md's ffmpeg verification
# and scripts/hash_check.py.
unit_hash = executable('unit_hash', 'unit/hash_test.c',
  dependencies : yah265_dep)
test('hash', unit_hash)

# Weighted prediction (phase 14 item 2): the pred_weight_table writer read
# back field by field by a hand-written parser of 7.3.6.3, the chroma
# offset through the DECODER's own derivation of it, 8.5.3.3.4.3 at hand
# values, and the identity that the whole escape rests on - at w = 1 <<
# denom the explicit clause IS the default prediction. Pure syntax and
# pure arithmetic, no encode; the decoder-side half is the two fade
# conformance vectors.
unit_wp = executable('unit_wp', 'unit/wp_test.c',
  dependencies : yah265_dep)
test('wp', unit_wp)
# Open GOP (phase 14 item 3): the CRA prediction barrier and the
# reference-list rule behind it, as a hand-written truth table out of
# 7.4.2.2, a hand-computed mini-GOP straddling a CRA, and a sweep against
# a second implementation written from the prose. Pure POC arithmetic, no
# encode; the decoder-side half is the seek check in
# scripts/conformance.sh, which cuts a stream at a CRA and feeds both
# decoders.
unit_gop = executable('unit_gop', 'unit/gop_test.c',
  dependencies : yah265_dep)
test('gop', unit_gop)

# PSNR, SSIM and the CSV writer (phase 14 item 4). The two metric
# reference numbers come from an independent walk of ffmpeg's own
# definitions over the same synthetic planes, not from this code; the
# encode-side half is the check against ffmpeg on a real clip recorded in
# docs/instruments.md.
unit_metrics = executable('unit_metrics', 'unit/metrics_test.c',
  dependencies : yah265_dep)
test('metrics', unit_metrics)

# The --qpfile and --zones grammars (phase 14 item 4), and in particular
# everything they REFUSE: a parser tested only on well-formed input is a
# parser that accepts anything.
unit_rcfile = executable('unit_rcfile', 'unit/rcfile_test.c',
  dependencies : yah265_dep)
test('rcfile', unit_rcfile)

# The two name tables of phase 14 item 15: --video-signal-type-preset's
# color sets and --tune's compositions, against the syntax element
# values and the knob values written out by hand. A name table is the one
# kind of code that ships with a typo and no symptom - a stream tagged
# with the wrong color description decodes, conforms and matches both
# decoders bit for bit.
unit_name = executable('unit_name', 'unit/name_test.c',
  dependencies : yah265_dep)
test('name', unit_name)

# One file per kernel family (checkasm.h has the layout): a family item
# adds its file here and one line to the table in main.c, and touches
# nothing else.
#
# Item 1 built this 8-BIT ONLY, because a 10-bit checkasm then had
# nothing but C to compare against C, which is the tautology the harness
# exists to avoid. Item 3 builds it at BOTH depths and makes the family
# list per depth: at 8 bits every family, at 10 bits exactly the
# families whose 16-bit lanes exist (meson.build's neon_families, or
# avx2_families on x86-64), so the harness never runs a leg that cannot
# fail. The macro a family file
# reads for its own presence is N265_CA_<FAMILY>, passed here rather than
# project-wide: it decides which files are IN THIS BINARY, while
# N265_NEON_<FAMILY> decides whether the NEON leg inside a compiled file
# has a kernel to check.
ca_families = ['transform', 'satd', 'mc', 'rdoq', 'cost', 'intra', 'deblock', 'sao']
if not is_8bit
  ca_families = neon_families + avx2_families
endif

checkasm_sources = ['checkasm/main.c']
checkasm_args = []
foreach f : ca_families
  checkasm_sources += 'checkasm/' + f + '.c'
  checkasm_args += '-DN265_CA_' + f.to_upper() + '=1'
endforeach

checkasm = executable('checkasm', checkasm_sources,
  c_args : checkasm_args,
  dependencies : yah265_dep)
# no --bench: the meson test is the correctness leg and stays fast.
# `make checkasm-bench` is where the tables come from.
# Rosetta 2 runs AVX2 but reports it only under this variable, which no
# real CPU's run reads (docs/x86-plan.md).
test('checkasm', checkasm, env : ['ROSETTA_ADVERTISE_AVX=1'])

# the BD integrator the board and the band both read through: a saturating
# point set must give a finite number, a well-conditioned one must give the
# same number it always did. Pure arithmetic, no encodes; skips (exit 77)
# where numpy is missing.
python3 = find_program('python3', required : false)
if python3.found()
  test('bd_rate', python3,
    args : [meson.project_source_root() / 'scripts' / 'bdcompare.py',
            '--selftest'])
endif

# The command line's HDR10+ JSON and Dolby Vision RPU readers. The ST
# 2094-40 writer is pinned to a profile B payload another encoder wrote.
unit_hdr_dyn = executable('unit_hdr_dyn',
  ['unit/hdr_dyn_test.c', '../cli/hdr_dyn.c'],
  include_directories : include_directories('../cli'))
test('hdr_dyn', unit_hdr_dyn)
