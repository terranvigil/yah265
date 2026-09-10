# One-word entry points for every gate. CI runs exactly these.

BUILD ?= build

.PHONY: build test conformance conformance-fast provenance asan determ determ-fast board board-quick clean

build:
	@[ -d $(BUILD) ] || meson setup $(BUILD)
	ninja -C $(BUILD)

test: build
	meson test -C $(BUILD) --print-errorlogs
	scripts/provenance_check.sh

conformance: build
	N265=$(BUILD)/n265 scripts/conformance.sh

conformance-fast: build
	N265=$(BUILD)/n265 scripts/conformance.sh --fast

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

# one clip, CTB 16 and 64 only: the shape of the gate at a tenth the wall,
# for use while bisecting. Not the gate.
determ-fast: build
	ENC=$(BUILD)/n265 CLIPS=sita_flat_720p CTBS='16 64' SAOS=off \
	    RCS=cqp scripts/determ_gate.sh

asan:
	@[ -d build-asan ] || meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
	ninja -C build-asan
	meson test -C build-asan --print-errorlogs
	N265=build-asan/n265 scripts/conformance.sh --fast

clean:
	rm -rf $(BUILD) build-asan
