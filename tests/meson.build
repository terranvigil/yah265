unit_bitstream = executable('unit_bitstream', 'unit/bitstream_test.c',
  dependencies : yah265_dep)
test('bitstream', unit_bitstream)

checkasm = executable('checkasm', 'checkasm/checkasm.c',
  dependencies : yah265_dep)
test('checkasm', checkasm)

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
