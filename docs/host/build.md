# Build the boot image

`cargo build --release` produces `target/release/spore`. A saved app image uses its embedded
kernel and EFI loader. A [direct boot](direct-boot.md) uses the embedded flat kernel.

**App image.** `spore build ./app -o app.img` packages an existing ELF directly.
For a package name such as `ant`, the CLI resolves the pinned archive and caches the binary.
The packager follows the ELF interpreter and library dependencies recursively.
Use `--lib-dir DIR` for additional AArch64 libraries and `--file HOST=/guest/path` for assets,
configuration, or libraries loaded by name at runtime. A missing required library fails the build.
The CLI packs the files, arguments, environment, working directory and network settings into
`/spore.app`. [App packer](../../tools/src/image/app.rs) · [Run an app](../app-images.md)

**Interactive image.** `spore build --userland` builds the Cargo `userland` feature and packages the interactive image. Add `--tests` for the integration image.
The image writer stages its files and links, installs musl and account files, and builds a separate
ext2 root disk. [Interactive manifest](../../userland/image.manifest) · [Boot a shell](../interactive.md)

[Compilers and build targets](toolchain.md) · [Launch in QEMU](qemu.md) · [Tests](checks.md) · [Release artifacts](release.md)
