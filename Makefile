BUILD_DIR ?= build
MESON ?= meson
CLANG_FORMAT ?= clang-format
QEMU_RUNNER ?= $(BUILD_DIR)/tools/spore-run
QEMU ?= qemu-system-aarch64
EDK2_VARS ?= $(BUILD_DIR)/edk2-vars.fd
RUN_CMD = cd "$(CURDIR)" && $(QEMU_RUNNER) --mode plain --timings --tmux-log-pane --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img" --qemu "$(QEMU)"

.PHONY: setup build image test-image runner test run run-tests run-shell-check format clean

setup:
	@test -d "$(BUILD_DIR)" || $(MESON) setup "$(BUILD_DIR)" --cross-file cross/aarch64-elf.ini

build: setup
	$(MESON) compile -C "$(BUILD_DIR)"

image: setup
	$(MESON) compile -C "$(BUILD_DIR)" image.img

test-image: setup
	$(MESON) compile -C "$(BUILD_DIR)" test_image.img

runner: setup
	$(MESON) compile -C "$(BUILD_DIR)" spore-run

test: build
	$(MESON) test -C "$(BUILD_DIR)"

run: image runner
	@if [ -n "$$TMUX" ]; then \
		$(RUN_CMD); \
	else \
		tmux new-session '$(RUN_CMD)'; \
	fi

run-tests: test-image runner
	$(QEMU_RUNNER) --mode filter --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/test_image.img"

run-shell-check: image runner
	$(QEMU_RUNNER) --mode shell --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img"

format:
	find bootloader kernel tests userland -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 $(CLANG_FORMAT) -i

clean:
	rm -rf "$(BUILD_DIR)"
