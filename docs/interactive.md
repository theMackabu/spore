# Boot a shell

After [building the CLI](../README.md), run these commands from the checkout.
The full image builds curl and the package manager from Git submodules.

```sh
git submodule update --init --recursive
./target/release/spore build --userland
./target/release/spore boot
```

Use `spore boot --user root` for a root console or `spore boot --user NAME` for another existing
account. The choice applies to that boot only and also works with `--wasm`.

The first boot creates a writable data disk backed by the current userland build. Spore
sees one filesystem. Rebuilding prepares a new base. To switch an existing installation,
shut down the guest with `sudo shutdown`, then run on the host:

```sh
./target/release/spore build --userland
./target/release/spore upgrade
./target/release/spore boot
```

An upgrade keeps local files, installed packages and their database. Unmodified defaults
follow the new base, including symlinks. When both sides changed a file, the local version
stays in place and the incoming version appears beside it with a `.spore-new` suffix.

The first upgrade from `run-root.ext2` works on a copy. If you still have the exact base
from which that disk was created, you can supply it with `spore upgrade --from-base PATH`.
Without that base, Spore keeps differing files and adds missing defaults. It cannot tell
which missing defaults you previously deleted.

`spore rollback` selects the previous installation, including its data as of the upgrade.
Each generation under `target/release/system/` holds its boot image, base and writable
overlay together. Keep those files together when making a backup.

Use `spore boot --snapshot` for a disposable session on the latest build or
`spore boot --root PATH` to boot a specific raw ext2 disk.

The full image includes offline manuals. Try `man ls`, `man 2 mmap`, or `man myc`. Press `q` to quit the pager.

[Disk selection](../tools/src/system.rs) · [Guest commands and services](../userland/README.md) · [QEMU runner](host/qemu.md)
