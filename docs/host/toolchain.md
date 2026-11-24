# What runs on the host and in the guest

The Nix development shell supplies the compilers, QEMU and disk-image tools. From the
checkout, `make` configures Meson with the AArch64 cross file and asks Ninja for the
`cli` target. [Development shell](../../tools/shell.nix) · [Makefile](../../Makefile) · [Cross file](../../cross/aarch64-elf.ini)

| Target | Build |
| --- | --- |
| Host CLI | Native Rust executable with a small C terminal helper |
| Kernel | Freestanding AArch64 C and assembly, linked with a Rust `no_std` archive |
| Guest programs | AArch64 Linux executables built against musl, or extracted from pinned packages |

The kernel archive supplies ELF, packet, path, policy and ext2 parsers. The kernel
provides its own memory routines and formatted logging. [Rust modules](../../kernel/lib.rs) · [Kernel build](../../kernel/meson.build) · [Memory routines](../../kernel/lib/mem.c) · [Guest packages](../userland/prebuilt.md)
