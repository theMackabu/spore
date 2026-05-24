BUILD_DIR ?= build
MESON ?= meson
CLANG_FORMAT ?= clang-format

.PHONY: setup build test run run-tests format clean

setup:
	@test -d "$(BUILD_DIR)" || $(MESON) setup "$(BUILD_DIR)" --cross-file cross/aarch64-elf.ini

build: setup
	$(MESON) compile -C "$(BUILD_DIR)"

test: build
	$(MESON) test -C "$(BUILD_DIR)"

run: build
	$(MESON) compile -C "$(BUILD_DIR)" run

run-tests: setup
	$(MESON) compile -C "$(BUILD_DIR)" run-tests

format:
	find bootloader kernel tests userland -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 $(CLANG_FORMAT) -i

clean:
	rm -rf "$(BUILD_DIR)"
