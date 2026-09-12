unit_bitstream = executable('unit_bitstream', 'unit/bitstream_test.c',
  dependencies : yah265_dep)
test('bitstream', unit_bitstream)

# The Annex A level table, checked at every level's own bound and one
# sample/frame/bit over it. It is the exhaustive half of the item 7 table
# check; scripts/level_check.sh is the half that actually encodes.
unit_level = executable('unit_level', 'unit/level_test.c',
  dependencies : yah265_dep)
test('level', unit_level)

# One file per kernel family (checkasm.h has the layout): a family item
# adds its file here and one line to the table in main.c, and touches
# nothing else.
#
# Item 1 built this 8-BIT ONLY, because a 10-bit checkasm then had
# nothing but C to compare against C, which is the tautology the harness
# exists to avoid. Item 3 builds it at BOTH depths and makes the family
# list per depth: at 8 bits every family, at 10 bits exactly the
# families whose 16-bit lanes exist (meson.build's neon_families), so
# the harness never runs a leg that cannot fail. The macro a family file
# reads for its own presence is N265_CA_<FAMILY>, passed here rather than
# project-wide: it decides which files are IN THIS BINARY, while
# N265_NEON_<FAMILY> decides whether the NEON leg inside a compiled file
# has a kernel to check.
ca_families = ['transform', 'satd', 'mc', 'rdoq', 'cost', 'intra']
if not is_8bit
  ca_families = neon_families
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
