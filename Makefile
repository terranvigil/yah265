# One-word entry points for every gate. CI runs exactly these.

BUILD ?= build
# Phase 8 item 1: the pixel is a compile-time type, so there is one
# LIBRARY per depth. Item 5a makes the binary one again: every build tree
# now compiles both libraries and `$(BUILD)/n265` links both and picks the
# encoder from the input's bit depth, so every target below - 8-bit and
# Main10 alike - names $(BUILD)'s binary.
#
# `build10` is still built and still tested. What it is now is the tree
# whose TESTS are Main10: checkasm at 16-bit lanes (`make checkasm10-*`)
# and the 10-bit unit tests, which is the one thing `-Dbit_depth=10` still
# decides. Its `n265-10` is byte-for-byte the same command line as
# `$(BUILD)/n265` and is kept only so an existing harness that names it
# keeps working.
BUILD10 ?= build10

.PHONY: build build8 build10 test smoke10 conformance conformance-fast conformance10 conformance10-fast conformance444 conformance444-fast conformance444-10 conformance444-10-fast provenance asan asan10 asan444 determ determ-fast determ10 determ444 board board-quick clean checkasm-bench checkasm-asan checkasm10-bench checkasm10-asan

build: build8 build10

build8:
	@[ -d $(BUILD) ] || meson setup $(BUILD)
	ninja -C $(BUILD)

build10:
	@[ -d $(BUILD10) ] || meson setup $(BUILD10) -Dbit_depth=10
	ninja -C $(BUILD10)

test: build
	meson test -C $(BUILD) --print-errorlogs
	scripts/provenance_check.sh
	scripts/symbol_check.sh $(BUILD) $(BUILD10)
	$(MAKE) smoke10

# The 10-bit leg of `make test`, and it is a SMOKE and not a correctness
# claim: the Main10 binary encodes an upshifted synthetic clip without
# crashing and ffmpeg decodes the result. What the samples are worth is
# phase 8 item 2's question, under the recon-match gate on both decoders.
smoke10: build
	N265_10=$(BUILD)/n265 scripts/smoke10.sh

# The kernel benchmark tables (ref / c / neon ns per call). `make test`
# runs checkasm for correctness only, which is what keeps it fast; this is
# where the numbers a kernel item reports come from. Wants a quiet box.
checkasm-bench: build
	$(BUILD)/tests/checkasm --bench

# checkasm under ASan+UBSan, part of the kernel item gate. The guard
# buffers catch a stray read or write at the BLOCK edge and the sanitiser
# catches it at the ALLOCATION edge; they miss different bugs, so a kernel
# item runs both.
checkasm-asan:
	@[ -d build-asan ] || meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
	ninja -C build-asan tests/checkasm
	build-asan/tests/checkasm

# The same instrument at Main10 (phase 8 item 3). checkasm is built at
# both depths now - `pixel` is uint8_t at 8 bits and uint16_t here, so a
# kernel is a different kernel at each - and the 10-bit binary holds
# exactly the families whose 16-bit lanes have been ported, which is why
# it can print fewer families than the 8-bit one and still be complete.
checkasm10-bench: build10
	$(BUILD10)/tests/checkasm --bench

checkasm10-asan:
	@[ -d build10-asan ] || meson setup build10-asan -Dbit_depth=10 \
	    -Db_sanitize=address,undefined -Db_lundef=false
	ninja -C build10-asan tests/checkasm
	build10-asan/tests/checkasm

conformance: build
	N265=$(BUILD)/n265 scripts/conformance.sh

conformance-fast: build
	N265=$(BUILD)/n265 scripts/conformance.sh --fast

# The Main10 list (phase 8 item 2): its own vectors, its own sources
# (C420p10), both decoders held bit-exact against --dump-recon. Separate
# from the two above on purpose - their pass counts are part of the 8-bit
# invariant and must not move. Since item 5a it is the same binary that
# runs them; the C420p10 tag is what selects the Main10 library.
conformance10: build
	N265_10=$(BUILD)/n265 scripts/conformance.sh --depth10

conformance10-fast: build
	N265_10=$(BUILD)/n265 scripts/conformance.sh --depth10 --fast

# The 4:4:4 lists (phase 8 item 6a), one per depth. The chroma format is a
# RUNTIME SPS property, so this is the same binary and the same library as
# every target above; what selects it is the source's C tag (C444 /
# C444p10). Its own list for the reason the Main10 one has its own: the
# 4:2:0 pass counts are part of the invariant and may not move.
conformance444: build
	N265=$(BUILD)/n265 scripts/conformance.sh --csp444

conformance444-fast: build
	N265=$(BUILD)/n265 scripts/conformance.sh --csp444 --fast

conformance444-10: build
	N265_10=$(BUILD)/n265 scripts/conformance.sh --csp444 --depth10

conformance444-10-fast: build
	N265_10=$(BUILD)/n265 scripts/conformance.sh --csp444 --depth10 --fast

provenance:
	scripts/provenance_check.sh

# append current goal numbers to docs/data/parity-history.csv (run at
# every feature milestone; the user charts this until parity)
board: build
	scripts/board.py

board-quick: build
	scripts/board.py --quick

# BD-PSNR rows only, speed columns blank: the phase 3 form (quality work
# doesn't move speed; the timed board runs at the phase exit and at every
# speed milestone from phase 4 on)
board-bd: build
	scripts/board.py --bd-only

# the sec 3 determinism matrix, crossed: CTB x SAO x RC x bframes x ref,
# threads 1/2/4/12 with t12 five times, under eight spinners
determ: build
	ENC=$(BUILD)/n265 scripts/determ_gate.sh

# Determinism at Main10 (phase 8 item 2). The clips are upshifted into a
# scratch directory first - the Main10 binary refuses 8-bit input - and
# the run is the reduced shape the item gate asks for: one clip, two
# thread counts, repeated. CORPUS10 overrides where the upshifted clips
# are put.
CORPUS10 ?= $(TMPDIR)yah265-corpus10
determ10: build
	@mkdir -p $(CORPUS10)
	@[ -s $(CORPUS10)/foreman_cif.y4m ] || \
	    scripts/upshift10.sh tests/corpus/foreman_cif.y4m $(CORPUS10)/foreman_cif.y4m
	ENC=$(BUILD)/n265 CORPUS=$(CORPUS10) CLIPS=foreman_cif \
	    THREADS="1 12" REFS=b1 RUNS=3 FRAMES=120 \
	    ARGS="--crf 28 --ctu 16 --ref 3" scripts/determ_repeat.sh

# one clip, CTB 16 and 64 only: the shape of the gate at a tenth the wall,
# for use while bisecting. Not the gate.
determ-fast: build
	ENC=$(BUILD)/n265 CLIPS=sita_flat_720p CTBS='16 64' SAOS=off \
	    RCS=cqp scripts/determ_gate.sh

# Determinism at 4:4:4 (phase 8 item 6a): one clip converted into a
# scratch directory first, then the item gate's reduced shape - two thread
# counts, repeated. CORPUS444 overrides where the converted clips go.
CORPUS444 ?= $(TMPDIR)yah265-corpus444
determ444: build
	@mkdir -p $(CORPUS444)
	@[ -s $(CORPUS444)/foreman_cif.y4m ] || \
	    ffmpeg -loglevel error -i tests/corpus/foreman_cif.y4m \
	        -pix_fmt yuv444p -strict -1 -f yuv4mpegpipe \
	        $(CORPUS444)/foreman_cif.y4m -y
	ENC=$(BUILD)/n265 CORPUS=$(CORPUS444) CLIPS=foreman_cif \
	    THREADS="1 12" REFS=b1 RUNS=2 FRAMES=120 \
	    ARGS="--crf 28 --ctu 32 --ref 3" scripts/determ_repeat.sh

asan:
	@[ -d build-asan ] || meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
	ninja -C build-asan
	meson test -C build-asan --print-errorlogs
	N265=build-asan/n265 scripts/conformance.sh --fast

# The Main10 conformance list under ASan + UBSan. Item 5a collapses this
# onto the SAME sanitiser tree `make asan` uses: build-asan compiles both
# libraries and its n265 links both, so the 10-bit code is instrumented
# there too and a second tree bought nothing. `make asan` is still the
# 8-bit read and still says nothing about 10.
asan10:
	@[ -d build-asan ] || meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
	ninja -C build-asan
	N265_10=build-asan/n265 scripts/conformance.sh --depth10 --fast

# The 4:4:4 vectors under ASan + UBSan, at both depths. Same tree as
# `make asan`: build-asan instruments both libraries and its n265 links
# both, and the chroma format needs no tree of its own at all.
asan444:
	@[ -d build-asan ] || meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
	ninja -C build-asan
	N265=build-asan/n265 scripts/conformance.sh --csp444 --fast
	N265_10=build-asan/n265 scripts/conformance.sh --csp444 --depth10 --fast

clean:
	rm -rf $(BUILD) $(BUILD10) build-asan build10-asan
