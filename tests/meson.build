unit_bitstream = executable('unit_bitstream', 'unit/bitstream_test.c',
  dependencies : yah265_dep)
test('bitstream', unit_bitstream)

# One file per kernel family (checkasm.h has the layout): a family item
# adds its file here and one line to the table in main.c, and touches
# nothing else.
checkasm_sources = [
  'checkasm/main.c',
  'checkasm/transform.c',
  'checkasm/satd.c',
  'checkasm/mc.c',
]

checkasm = executable('checkasm', checkasm_sources,
  dependencies : yah265_dep)
# no --bench: the meson test is the correctness leg and stays fast.
# `make checkasm-bench` is where the tables come from.
test('checkasm', checkasm)
