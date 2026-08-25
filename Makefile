# One-word entry points for every gate. CI runs exactly these.

BUILD ?= build

.PHONY: build test conformance conformance-fast provenance asan board board-quick clean

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

asan:
	@[ -d build-asan ] || meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
	ninja -C build-asan
	meson test -C build-asan --print-errorlogs
	N265=build-asan/n265 scripts/conformance.sh --fast

clean:
	rm -rf $(BUILD) build-asan
