# Spore Userland

The interactive image ships standalone GNU coreutils from Void Linux’s AArch64
musl package. Each command keeps its own executable path for confinement.
Use `df -h` for human-readable sizes, `df -m` for MiB, or `df -a` to include
zero-size filesystems. Spore builds its shell, system services, and remaining
local commands against musl.

## Third-party programs

Void Linux also supplies grep, findutils, procps-ng, util-linux, file, GNU make,
and GNU ed. `edit` starts ed, `pico` starts nano, and `mk` starts GNU make.
Toybox provides ping and top. Alpine BusyBox provides nslookup. The hostname command uses net-tools and obtains
the interface address for `-I` from Spore’s network configuration.

Alpine supplies sudo, shadow’s su, passwd, useradd and userdel, and Linux-PAM.
`mkuser NAME` creates a home and private group through useradd. `deluser NAME`
invokes userdel and protects the root account. Use `sudo passwd NAME` to set
an account password. On the serial console, sudo runs commands on the current
terminal.

The interactive image uses Alpine AArch64 packages for dash, fastfetch, Git, jq, htop, nano,
Vim, TinyCC, GNU wget, GNU which and the Dropbear SSH client and key tools.
Alpine supplies wide-character ncurses. The narrow-character library comes
from Void Linux’s AArch64 musl package. The host uses zstd to unpack it. The serial
console uses Alpine’s xterm-256color terminal description. Ant comes from its
upstream musl release.
`prebuilt/packages.json` pins each archive URL and SHA-256 and lists the files
to extract. The image manifest sets their guest paths and shared-library links. The first build downloads the
archives. Subsequent builds reuse verified copies from `.cache/packages/`,
including after `make clean`.

The build also downloads target headers and static libraries. Spore's patched
curl and trustd link against mbedTLS 4. The patched apk-tools (spm) links
against mbedTLS 3 and zlib in a separate build directory. These build-time
headers and archives stay outside the guest image.

## Layout

- `lib/spore/`: tiny shared helper runtime used by tools.
- `bin/<tool>/main.c`: real commands baked into `/bin`; mycelium init is baked
  as `/lib/mycelium/init` and reached through `/sbin/init`.
- `libexec/spore-test/<instrument>/main.c`: regression instruments baked to
  `/libexec/spore-test`; this directory is not in `PATH`.
- `demos/<name>/main.c`: confinement fixtures baked into `/home/spore/demos`.
- `tests/integration/`: regression binaries used only by `run-tests`.
- `image.manifest`: declarative list of sources and baked paths for the
  interactive image.

## Placement Rule

Placement follows the invoker, not how large or "tool-like" a program feels.
Human-interactive commands live under `userland/bin` and bake to `/bin`.
Regression instruments invoked by `run-tests`, `run-shell-check`, or another
harness live under `userland/libexec/spore-test`, bake to `/libexec/spore-test`,
and must be called by full path. Curiosity fixtures live under `userland/demos`
and bake to `/home/spore/demos`.

## Add a Tool

1. Create `userland/bin/<name>/main.c`.
2. Include `spore.h` when you want shared helpers such as `eprintf`,
   `usage`, `basename`, or `streq`.
3. Add `userland/bin/<name>/meson.build` with a `custom_target` matching the
   neighboring tools.
4. Add `subdir('bin/<name>')` to `userland/meson.build`.
5. Add `userland/bin/<name> /bin/<name>` to `userland/image.manifest`.
6. Run:

```sh
make build
make run
```

Use `make test` for host tests and `make run-tests` for the scripted regression
image.

Commands should return `0` on success, `1` for ordinary runtime failure, and
`2` for usage errors. Keep diagnostics on stderr unless the command's normal
contract is to print a result.

## Add a Confinement Demo

1. Create `userland/demos/<name>/main.c`.
2. Add a `manifest` file describing the policy fixture.
3. Add `userland/demos/<name> /home/spore/demos/<name>` to `userland/image.manifest`.
4. Update `tools/src/runner/shell_check.c` if the demo is part of the policy gate.

If a harness depends on the binary, bake it to `/libexec/spore-test` instead of
the demos directory.

## Regression Image

Interactive boot stays lean. The old boot-time gauntlet lives in
`userland/tests/integration` and runs through:

```sh
make run-tests
```
