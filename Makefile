BUILD_DIR ?= build
MESON ?= meson
NINJA ?= ninja

.PHONY: cli run test run-tests run-shell-check format
cli:
	@test -f "$(BUILD_DIR)/build.ninja" || $(MESON) setup "$(BUILD_DIR)" --cross-file cross/aarch64-elf.ini
	$(NINJA) -C "$(BUILD_DIR)" cli

run test run-tests run-shell-check: cli
	"$(BUILD_DIR)/spore" $(command-$@)
command-run = boot $(if $(RUN_ROOT),--root "$(RUN_ROOT)") $(if $(EDK2_VARS),--vars "$(EDK2_VARS)")
command-test = test host
command-run-tests = test guest
command-run-shell-check = test shell

format:
	find kernel tests tools/src userland -path userland/third_party -prune -o -type f \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 clang-format -i
	git ls-files -z -- 'kernel/*.rs' 'tools/*.rs' 'userland/*.rs' | xargs -0 rustfmt --edition 2024
