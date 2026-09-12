unit_bitstream = executable('unit_bitstream', 'unit/bitstream_test.c',
  dependencies : yah265_dep)
test('bitstream', unit_bitstream)

# One file per kernel family (checkasm.h has the layout): a family item
# adds its file here and one line to the table in main.c, and touches
# nothing else.
#
# 8-BIT ONLY since phase 8 item 1. checkasm compares a NEON kernel
# against its C reference, and there are no 16-bit-lane kernels to
# compare until item 3 ports them: the 10-bit library runs pure C by
# construction (the arm/ translation units are not in that build at
# all), so a 10-bit checkasm would be C against C, which is the
# tautology the harness exists to avoid. Item 3 builds it at 10 and adds
# its own leg to the gate.
checkasm_sources = [
  'checkasm/main.c',
  'checkasm/transform.c',
  'checkasm/satd.c',
  'checkasm/mc.c',
  'checkasm/rdoq.c',
  'checkasm/cost.c',
  'checkasm/intra.c',
]

if is_8bit
  checkasm = executable('checkasm', checkasm_sources,
    dependencies : yah265_dep)
  # no --bench: the meson test is the correctness leg and stays fast.
  # `make checkasm-bench` is where the tables come from.
  test('checkasm', checkasm)
endif

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
