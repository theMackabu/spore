# Programs in the interactive image

Void supplies standalone GNU coreutils, grep, findutils, procps-ng, util-linux, file, make
and ed. Separate executable paths let filesystem policies distinguish individual commands.
Toybox provides ping and top. Alpine supplies BusyBox nslookup, Bash, Git, jq, htop, nano,
Vim, TinyCC, wget, which, fastfetch and Dropbear client tools. Ant comes from its upstream
musl release. `edit`, `pico` and `mk` start ed, nano and make. [Installed paths](../../userland/image.manifest)

Use `df -h` for human-readable sizes, `df -m` for MiB, or `df -a` to include zero-size
filesystems. `hostname -I` prints the address from the guest's [network configuration](network.md).

Alpine supplies sudo, shadow and PAM. At the console, `sudo mkuser NAME` creates an account, home
and private group, then prompts for a password. Use `su - NAME` to log in. For an existing account,
set its password with `sudo passwd NAME`. Use `sudo mkuser --no-password NAME` to create an account
with password login locked. An administrator can enter it with `sudo su - NAME`.
`sudo deluser NAME` removes an account and protects root. [Account defaults](../../userland/etc/login.defs) · [Account removal](../../userland/bin/deluser.sh)

For additional packages inside the guest, `spm` is Spore's build of apk-tools. The static build includes upstream's package-authoring commands.
It fetches URLs through curl and reads the configured Alpine repositories. [Package manager patches](../../userland/usr/local/bin/apk/build.sh) · [Repositories](../../userland/etc/apk/repositories)

[How the host assembles packages](prebuilt.md) · [Guest shell](shell.md)
