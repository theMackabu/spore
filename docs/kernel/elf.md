# Loading an executable

The loader reads the AArch64 ELF headers through the VFS and records the loadable segments as
virtual memory areas. When the executable names an interpreter in `PT_INTERP`, the loader
opens that path in the guest and maps the interpreter too. Execution then starts at the
interpreter's entry point. [ELF parsing](../../kernel/exec/elf.rs) · [Program loading](../../kernel/exec/program.c)

The initial user stack holds arguments, environment strings and the Linux auxiliary vector.
It gives the runtime the main executable's entry address, program headers, interpreter base
and random bytes. The first instruction fetch can bring an executable page into memory on demand.
[Stack setup](../../kernel/exec/stack.c) · [Page faults](../memory/faults.md)

[How libraries enter the image](../host/build.md) · [First process](../../kernel/init/process.c)
