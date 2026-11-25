# Programs in the interactive image

Void supplies standalone GNU coreutils, grep, findutils, procps-ng, util-linux, file, make
and ed. Separate executable paths let filesystem policies distinguish individual commands.
Toybox provides ping and top. Alpine supplies BusyBox nslookup, dash, Git, jq, htop, nano,
Vim, TinyCC, wget, which, fastfetch and Dropbear client tools. Ant comes from its upstream
musl release. `edit`, `pico` and `mk` start ed, nano and make. [Installed paths](../../userland/image.manifest)

Use `df -h` for human-readable sizes, `df -m` for MiB, or `df -a` to include zero-size
filesystems. `hostname -I` prints the address from the guest's [network configuration](network.md).

Alpine supplies sudo, shadow and PAM. `sudo mkuser NAME` creates a home and private group,
`sudo passwd NAME` sets a password, and `sudo deluser NAME` removes the account. The wrapper
protects root. [Account defaults](../../userland/etc/login.defs) · [Account removal](../../userland/bin/deluser.sh)

For additional packages inside the guest, `spm` is Spore's build of apk-tools. The static build includes upstream's package-authoring commands.
It fetches URLs through curl and reads the configured Alpine repositories. [Package manager patches](../../userland/usr/local/bin/apk/build.sh) · [Repositories](../../userland/etc/apk/repositories)

On AArch64, `codex-install` downloads and verifies the pinned Codex release. If you already
downloaded the complete package, pass it to the installer. Run these commands inside Spore.

```sh
codex-install codex-package-aarch64-unknown-linux-musl.tar.gz
~/.local/bin/codex app-server daemon start
~/.local/bin/codex app-server daemon version
```

[How the host assembles packages](prebuilt.md) · [Guest shell](shell.md)
