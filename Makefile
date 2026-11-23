BUILD_DIR ?= build
MESON ?= meson
NINJA ?= ninja
CLANG_FORMAT ?= clang-format
QEMU_RUNNER ?= $(BUILD_DIR)/tools/spore-run
QEMU ?= $(or $(wildcard /opt/homebrew/bin/qemu-system-aarch64),qemu-system-aarch64)
RUN_QEMU = $(QEMU_RUNNER) --qemu "$(QEMU)"
EDK2_VARS ?= $(BUILD_DIR)/edk2-vars.fd
RUN_ROOT ?= $(BUILD_DIR)/run-root.ext2
TEST_RUN_ROOT ?= $(BUILD_DIR)/test-run-root.ext2
RUN_CMD = cd "$(CURDIR)" && $(RUN_QEMU) --mode plain --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img" --root "$(RUN_ROOT)"

.DEFAULT_GOAL := all

.PHONY: all setup build image test-image runner run-root reset-disk test-run-root test run-reset run run-tests run-shell-check benchmark bench-inodes rng-smoke kill format fetch clean

all: image test-image runner

setup:
	@test -f "$(BUILD_DIR)/build.ninja" || $(MESON) setup "$(BUILD_DIR)" --cross-file cross/aarch64-elf.ini

build: setup
	$(NINJA) -C "$(BUILD_DIR)"

image: setup
	$(NINJA) -C "$(BUILD_DIR)" image.img

test-image: setup
	$(NINJA) -C "$(BUILD_DIR)" test_image.img

runner: setup
	$(NINJA) -C "$(BUILD_DIR)" tools/spore-run

run-root: image
	@if [ ! -f "$(RUN_ROOT)" ]; then \
		cp -f "$(BUILD_DIR)/root.ext2" "$(RUN_ROOT)"; \
		echo "created persistent run disk: $(RUN_ROOT)"; \
	elif [ "$(BUILD_DIR)/root.ext2" -nt "$(RUN_ROOT)" ]; then \
		cp -f "$(BUILD_DIR)/root.ext2" "$(RUN_ROOT)"; \
		echo "reseeded persistent run disk from newer base image: $(RUN_ROOT)"; \
	else \
		echo "using persistent run disk: $(RUN_ROOT)"; \
	fi

reset-disk: image
	cp -f "$(BUILD_DIR)/root.ext2" "$(RUN_ROOT)"
	@echo "reset persistent run disk: $(RUN_ROOT)"

test-run-root: test-image
	cp -f "$(BUILD_DIR)/test_root.ext2" "$(TEST_RUN_ROOT)"

test: build
	$(MESON) test -C "$(BUILD_DIR)"

run: runner run-root
	$(RUN_CMD)

run-reset: runner image
	cp -f "$(BUILD_DIR)/root.ext2" "$(RUN_ROOT)"
	@echo "reset persistent run disk: $(RUN_ROOT)"
	$(RUN_CMD)

run-tests: runner test-image
	$(RUN_QEMU) --mode integration --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/test_image.img" --snapshot-root --root "$(BUILD_DIR)/test_root.ext2"

run-shell-check: runner image
	$(RUN_QEMU) --mode shell --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img" --snapshot-root --root "$(BUILD_DIR)/root.ext2"

benchmark: runner image
	@root="$$(mktemp "$(BUILD_DIR)/bench-root.XXXXXX.ext2")"; \
	cp -f "$(BUILD_DIR)/root.ext2" "$$root"; \
	trap 'rm -f "$$root"' EXIT INT TERM; \
	$(RUN_QEMU) --mode bench --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img" --root "$$root"

bench-inodes: setup
	$(NINJA) -C "$(BUILD_DIR)" tests/bench_ext2_inodes
	"$(BUILD_DIR)/tests/bench_ext2_inodes"

rng-smoke: runner image
	@set -e; out1="$$(mktemp "$(BUILD_DIR)/rng-a.XXXXXX.txt")"; \
	out2="$$(mktemp "$(BUILD_DIR)/rng-b.XXXXXX.txt")"; \
	trap 'rm -f "$$out1" "$$out2"' EXIT INT TERM; \
	$(RUN_QEMU) --mode rng --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img" --snapshot-root --root "$(BUILD_DIR)/root.ext2" > "$$out1"; \
	$(RUN_QEMU) --mode rng --vars "$(EDK2_VARS)" --image "$(BUILD_DIR)/image.img" --snapshot-root --root "$(BUILD_DIR)/root.ext2" > "$$out2"; \
	cat "$$out1"; \
	cat "$$out2"; \
	if ! grep -q '^\[rng\] 00000000:' "$$out1" || ! grep -q '^\[rng\] 00000000:' "$$out2"; then \
		echo "rng-smoke: FAIL missing /dev/urandom sample"; \
		exit 1; \
	fi; \
	if cmp -s "$$out1" "$$out2"; then \
		echo "rng-smoke: FAIL repeated /dev/urandom sample"; \
		exit 1; \
	fi; \
	echo "rng-smoke: PASS samples differ across boots"

kill:
	@pkill -f 'qemu-system-aarch64.*(image\.img|root\.ext2|edk2-vars\.fd)' >/dev/null 2>&1 || true
	@pkill -f '(^|/)spore-run( |$$)' >/dev/null 2>&1 || true

format:
	find kernel tests tools/src userland \( -path userland/third_party -o -path userland/third_party/\* \) -prune -o -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 $(CLANG_FORMAT) -i

fetch:
	onefetch -e userland/third_party

clean:
	rm -rf "$(BUILD_DIR)"
