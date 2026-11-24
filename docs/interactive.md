# Boot a shell

After [building the CLI](../README.md), run these commands from the checkout.
The full image builds curl and the package manager from Git submodules.

```sh
git submodule update --init --recursive
./target/release/spore build --userland
./target/release/spore boot
```

The first boot copies the base filesystem from `target/release/root.ext2` to
`target/release/run-root.ext2`. Later boots reuse that writable copy. For a disposable
session on the base image, run `./target/release/spore boot --snapshot`. Inside the shell,
`sudo shutdown` ends the session. To use an existing disk, pass its path with `--root`, for example `./target/release/spore boot --root build/run-root.ext2`.

The full image includes offline manuals. Try `man ls`, `man 2 mmap`, or `man myc`. Press `q` to quit the pager.

[Disk selection](../tools/src/system.rs) · [Guest commands and services](../userland/README.md) · [QEMU runner](host/qemu.md)
