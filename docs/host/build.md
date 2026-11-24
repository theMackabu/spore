# Build the boot image

`make` uses Meson and Ninja to build the native CLI, the AArch64 kernel, and the Limine EFI loader.
Plain `./build/spore build` runs Ninja’s `cli` target.
For saved images the CLI puts the kernel and loader on a FAT boot image. The build also
produces a flat kernel for [direct boot](direct-boot.md). [Build targets](../../meson.build) · [Image writer](../../tools/src/image/mod.rs)

**App image.** `spore build ./app -o app.img` packages an existing ELF directly.
For a package name such as `ant`, the CLI first asks Ninja for the prebuilt binary.
The packager follows the ELF interpreter and library dependencies recursively.
Use `--lib-dir DIR` for additional AArch64 libraries and `--file HOST=/guest/path` for assets,
configuration, or libraries loaded by name at runtime. A missing required library fails the build.
The CLI packs the files, arguments, environment, working directory and network settings into
`/spore.app`. [App packer](../../tools/src/image/app.rs) · [Run an app](../app-images.md)

**Interactive image.** `spore build --userland` enables the Meson option if needed and runs Ninja to build the image.
Meson selects a manifest for the shell or the guest test suite.
The image writer stages its files and links, installs musl and account files, and builds a separate
ext2 root disk. [Image rules](../../images/meson.build) · [Interactive manifest](../../userland/image.manifest) · [Boot a shell](../interactive.md)

[Compilers and build targets](toolchain.md) · [Launch in QEMU](qemu.md) · [Tests](checks.md) · [Release artifacts](release.md)
