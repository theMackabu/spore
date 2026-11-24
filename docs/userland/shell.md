# The interactive shell

The console service starts `/bin/msh` as `spore`. At startup msh reads `/etc/profile`,
changes to the home directory, loads history, then reads `~/.profile` and `~/.mshrc`
when present. Use `msh -c 'command'` to evaluate a line or `msh file` to run a script.
The image also provides dash as `/bin/sh`. [Shell entry](../../userland/bin/msh/main.c) · [Console unit](../../userland/etc/mycelium/console.service)

The lexer and parser turn input into commands for the evaluator. For a foreground job,
the shell assigns the child a process group and hands it the terminal. When the job ends,
the shell takes the terminal back. [Parser](../../userland/bin/msh/parser.c) · [Evaluator](../../userland/bin/msh/eval.c) · [Job control](../../userland/bin/msh/job.c)

[Terminal input](../kernel/terminal.md) · [Run a confined command](../processes/confinement.md) · [Installed programs](packages.md)
