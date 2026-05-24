BUILD_DIR ?= build
MESON ?= meson
CLANG_FORMAT ?= clang-format
QEMU_RUNNER ?= $(BUILD_DIR)/tools/spore-run
QEMU ?= qemu-system-aarch64
EDK2_VARS ?= $(BUILD_DIR)/edk2-vars.fd
RUN_ROOT ?= $(BUILD_DIR)/run-root.ext2
TEST_RUN_ROOT ?= $(BUILD_DIR)/test-run-root.ext2
RUN_CMD = cd "$(CURDIR)" && $(QEMU_RUNNER) --mode plain --timings --tmux-log-pane --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img" --root "$(RUN_ROOT)" --qemu "$(QEMU)"

.DEFAULT_GOAL := all

.PHONY: all setup build image test-image runner run-root test-run-root test run run-tests run-shell-check kill format clean

all: image test-image runner

setup:
	@if [ -d "$(BUILD_DIR)" ] && [ ! -f "$(BUILD_DIR)/build.ninja" ]; then \
		rm -rf "$(BUILD_DIR)"; \
	fi
	@test -d "$(BUILD_DIR)" || $(MESON) setup "$(BUILD_DIR)"

build: setup
	$(MESON) compile -C "$(BUILD_DIR)"

image: setup
	$(MESON) compile -C "$(BUILD_DIR)" image.img

test-image: setup
	$(MESON) compile -C "$(BUILD_DIR)" test_image.img

runner: setup
	$(MESON) compile -C "$(BUILD_DIR)" spore-run

run-root: image
	cp -f "$(BUILD_DIR)/root.ext2" "$(RUN_ROOT)"

test-run-root: test-image
	cp -f "$(BUILD_DIR)/test_root.ext2" "$(TEST_RUN_ROOT)"

test: build
	$(MESON) test -C "$(BUILD_DIR)"

run: runner run-root
	@if [ -n "$$TMUX" ]; then \
		$(RUN_CMD); \
	else \
		tmux new-session '$(RUN_CMD)'; \
	fi

run-tests: runner test-run-root
	@root="$$(mktemp "$(BUILD_DIR)/run-tests-root.XXXXXX.ext2")"; \
	cp -f "$(BUILD_DIR)/test_root.ext2" "$$root"; \
	trap 'rm -f "$$root"' EXIT INT TERM; \
	$(QEMU_RUNNER) --mode filter --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/test_image.img" --root "$$root"

run-shell-check: runner run-root
	@root="$$(mktemp "$(BUILD_DIR)/run-shell-root.XXXXXX.ext2")"; \
	cp -f "$(BUILD_DIR)/root.ext2" "$$root"; \
	trap 'rm -f "$$root"' EXIT INT TERM; \
	$(QEMU_RUNNER) --mode shell --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img" --root "$$root"

kill:
	@pkill -f 'qemu-system-aarch64.*(image\.img|root\.ext2|edk2-vars\.fd)' >/dev/null 2>&1 || true
	@pkill -f '(^|/)spore-run( |$$)' >/dev/null 2>&1 || true

format:
	find bootloader kernel tests userland -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 $(CLANG_FORMAT) -i

clean:
	rm -rf "$(BUILD_DIR)"
