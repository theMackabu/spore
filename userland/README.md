# Spore Userland

Spore userland is organized as one static musl binary per program. That keeps
each tool independently confineable by the policy layer.

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
4. Update `tools/src/spore_run.c`'s shell mode if the demo is part of the policy gate.

If a harness depends on the binary, bake it to `/libexec/spore-test` instead of
the demos directory.

## Regression Image

Interactive boot stays lean. The old boot-time gauntlet lives in
`userland/tests/integration` and runs through:

```sh
make run-tests
```
