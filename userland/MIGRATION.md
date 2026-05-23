# Userland Cleanup Migration

## Phase 0 Decision

Spore will use one static musl binary per tool, not a busybox-style multiplexer.

Reason: separate binaries are independently confineable. A future policy can give
`cat` a different manifest than `ls`, which matches Spore's capability model. The
tradeoff is image size because each binary carries static musl; shrinking can be a
later dynamic-linking or multiplexer goal. The current filesystem has no symlinks,
so a multiplexer would also need hardlink-style dispatch or shell support.

## Current Binary Disposition

Real tools, target `/bin`:

- `spsh`
- `ls`
- `cat`
- `echo`
- `mkdir`
- `rm`
- `touch`
- `pwd`
- `true`
- `false`
- `hello`

Confinement fixtures, target `/demos`:

- `spinner`
- `peeker`
- `writer`
- `memhog`
- `escalate`

Integration-only checks, remove from image after migrating assertions:

- `spore_demo`
- `exec_child`

Duplication to resolve:

- Keep canonical `/bin/hello`.
- Drop `/hello` and `/boot/hello` from the baked image.

## Known Test Debt

The v2 regression log currently shows `stdin demo: blocking read resume: SKIP`
unless driven by the special stdin harness. Closing that gap is explicitly outside
this cleanup goal; preserve the note while moving the regression binary.
