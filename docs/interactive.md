# Boot a shell

After [building the CLI](../README.md), run these commands from the checkout.
The full image builds curl and the package manager from Git submodules.

```sh
git submodule update --init --recursive
./build/spore build --userland
./build/spore boot
```

The first boot copies the base filesystem from `build/root.ext2` to
`build/run-root.ext2`. Later boots reuse that writable copy. For a disposable
session on the base image, run `./build/spore boot --snapshot`. Inside the shell,
`sudo shutdown` ends the session.

[Disk selection](../tools/src/system.rs) · [Guest commands and services](../userland/README.md) · [QEMU runner](host/qemu.md)
