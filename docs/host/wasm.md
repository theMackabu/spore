# Run ARM64 programs in WebAssembly

Spore’s experimental Wasm backend runs ARM64 apps and boots the full Spore userland from an ext2 or ext2+ root disk.
The kernel and Rust instruction interpreter share one WebAssembly module. Guest instruction
fetches, loads and stores use Spore’s user page tables. Linux syscalls enter Spore’s shared
dispatcher. The host provides console output, time, entropy and block I/O.

Build and run from the development shell.

```sh
cargo build --release
./target/release/spore build --wasm
./target/release/spore run --wasm ./busybox.static -- echo hello
```

Use Alpine’s **aarch64 busybox-static** package for the example. Extract
`bin/busybox.static` and pass that executable to the runner. Spore also runs static PIE
executables. The CLI installs the executable under its filename in `/app`, which lets BusyBox
select its applet. [Alpine package](https://pkgs.alpinelinux.org/package/v3.23/main/aarch64/busybox-static)

Pass an ext2 or ext2+ disk to read its files under `/data`.

```sh
./target/release/spore run --wasm ./busybox.static --data target/release/root.ext2 -- cat /data/etc/os-release
```

## Full userland

```sh
./target/release/spore build --userland
./target/release/spore boot --wasm
```

The kernel launches `/sbin/init` from `target/release/root.ext2`. To select another root, pass `--root path/to/root.ext2`. Each boot keeps disk writes in a private in-memory snapshot and discards them on exit. Type commands at the guest shell or press Ctrl+] to disconnect.

## Browser

The build also produces a standalone browser runner in `target/release/wasm-web/`. Serve that directory locally.

```sh
python3 -m http.server 8000 --bind 127.0.0.1 --directory target/release/wasm-web
```

Open `http://localhost:8000/`, select a static ELF and enter one argument per
line. For a dynamically linked ELF, package its interpreter, libraries, files and
arguments on the host and select the resulting payload.

```sh
./target/release/spore build --wasm ./my-app -o my-app.spore --lib-dir ./libs -- --version
```

The CLI performs this packaging automatically on `spore run --wasm`. A worker runs the guest locally and reads ranges from the selected disk on demand.
To boot a system, select only the root disk, click Run and send commands through Console input. Stop terminates the worker and discards its snapshot. Browsers and Node can also import [the launcher](../../tools/wasm/run.mjs).

## Current scope

One CPU runs guest threads with 64 MiB of guest RAM. The shared Spore scheduler preempts threads at 100 Hz and the launcher yields while they sleep. The interpreter handles a subset
of A64 integer, branch, load/store, exclusive and SIMD instructions. It caches instruction
classes and address translations. It reports the guest PC on an unsupported instruction or
memory fault. The launcher defaults to a budget of 100 million guest instructions.

Linux syscalls use the native AArch64 dispatcher, including its seccomp checks. The appliance profile allows threads and denies additional processes. Batch apps without console input see EOF. Full-system boots use Spore’s TTY. External networking and JIT workloads are still unsupported.
Ant currently stops on an unsupported ARM64 instruction during startup. Claude Code is unverified.

`spore test wasm` builds an ordinary musl ELF and compares its output and exit status
under QEMU and Wasm. It checks pthreads, futex wakeups, timer preemption, thread-local storage and appliance policy. It also exercises cross-page accesses, permission changes,
unsupported instructions and the execution budget. `spore bench wasm` times an ARM64
integer loop using Node with an already compiled module and a fresh instance per run.
