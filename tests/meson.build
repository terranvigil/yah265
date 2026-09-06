unit_bitstream = executable('unit_bitstream', 'unit/bitstream_test.c',
  dependencies : yah265_dep)
test('bitstream', unit_bitstream)

checkasm = executable('checkasm', 'checkasm/checkasm.c',
  dependencies : yah265_dep)
test('checkasm', checkasm)
