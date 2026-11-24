# What runs on the host and in the guest

Run `cargo build --release` in the development shell. The CLI embeds its kernel and EFI loader. Running a cached static app needs QEMU. Fetching a named package uses curl and bsdtar.
[Development shell](../../tools/shell.nix) · [Cargo workspace](../../Cargo.toml)

| Target | Build |
| --- | --- |
| Host CLI | Native Rust executable with a small C terminal helper |
| Kernel | Freestanding AArch64 C and assembly, linked with a Rust `no_std` archive |
| Guest programs | AArch64 Linux executables built against musl, or extracted from pinned packages |

The kernel archive supplies ELF, packet, path, policy and ext2 parsers. The kernel
provides its own memory routines and formatted logging. [Rust modules](../../kernel/lib.rs) · [Kernel build](../../kernel/build.rs) · [Memory routines](../../kernel/lib/mem.c) · [Guest packages](../userland/prebuilt.md)
