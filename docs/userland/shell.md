# The interactive shell

Mycelium starts Bash as a login shell in the console user's home directory. Starting
another Bash keeps the current directory. The image also provides Bash as `/bin/sh`.

Put interactive aliases and prompt settings in `~/.bashrc`. The default login profile
loads it for interactive Bash sessions. After editing, run `source ~/.bashrc`.

[Console unit](../../userland/etc/mycelium/console.service) · [Terminal input](../kernel/terminal.md) · [Run a confined command](../processes/confinement.md)
