# Run the checks

From the checkout, use `./build/spore test SUITE`. Build the [interactive image](../interactive.md)
first for the guest, shell and RNG suites.

| Suite | What it exercises |
| --- | --- |
| `host` | Native Meson tests for kernel components, ABI layouts and process models |
| `guest` | Integration programs booted with their own root filesystem |
| `shell` | Commands and services driven through the interactive serial console |
| `rng` | Random samples compared across guest boots |
| `appliance` | Static and dynamic musl probes and two Ant images using one ext2 volume |
| `stress` | Repeated static and dynamic app launches with varied guest RAM and seeds |

Use `./build/spore bench guest` for guest benchmarks, `bench inodes` for ext2 allocation,
`bench startup` for CLI launch to the Ant prompt, and `bench appliance` for the
[app boot sweep](measurements.md).

[Suite selection](../../tools/src/system.rs) · [Host tests](../../tests/meson.build) · [Guest tests](../../userland/tests/integration/main.c) · [Shell checks](../../tools/src/runner/commands.rs)
