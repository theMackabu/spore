# Spore and kwemu

The 25,000-line BusyBox pipeline took 299 ms on Spore and 349 ms on kwemu. Both produced the same checksums and child statuses. The integer-loop and fork/wait timings overlapped. kwemu was faster on the musl syscall wrapper, cached reads and memory copies.

| Workload | Spore, ms | unkernel with kwemu, ms |
|---|---:|---:|
| Fresh kernel instance to first PID 1 write | 6.38 (6.24–6.67) | 14.60 (12.97–16.10) |
| 16,777,216 ARM64 integer loop instructions | 1.47 (1.17–3.14) | 1.50 (1.47–1.56) |
| 100,000 calls through musl’s generic `syscall(SYS_getpid)` wrapper | 16.90 (13.74–19.96) | 8.81 (8.68–9.92) |
| 100,000 direct `svc` calls to `getpid` | 5.90 (5.16–6.89) | 4.62 (4.51–4.71) |
| 100,000 cached 64-byte `pread` calls on tmpfs | 26.09 (25.56–26.43) | 15.93 (15.25–16.66) |
| 128 MiB copied with musl `memcpy` | 11.03 (10.87–13.63) | 9.47 (9.41–9.51) |
| 200,000 ARM SHA256H/SHA256H2 instruction pairs | 2.35 (2.11–3.81) | 3.07 (3.02–3.08) |
| 200 fork, child exit and parent wait cycles | 4.67 (4.54–4.83) | 4.69 (4.26–4.86) |
| BusyBox reverse-sort and SHA-256 pipeline, 25,000 lines | 299.02 (289.60–321.74) | 348.62 (338.80–350.77) |

Values are medians, with the observed minimum and maximum in parentheses. The runner discarded one warm-up per engine, then alternated engine order over five measured runs each.

## What was measured

The host was an Apple M5 Pro with 64 GB of RAM, macOS 27.0.1 and Node 24.15.0. Each engine booted a fresh copy of the same 16 MiB ext2 disk from host RAM with one virtual CPU, a 128 MiB memory setting, networking disabled and its JIT enabled. The disk contained one static AArch64 musl PID 1 built with GCC 15.2.0 and Alpine’s AArch64 BusyBox 1.37.0-r30. The guest ran `seq 1 25000 | sort -r | sha256sum` with all three applets from that BusyBox build. The runner checked its digest against an independent host sort. [Guest workload](../../tests/wasm/workload.c) · [Runner](../../tests/wasm/compare.mjs)

Boot timing starts immediately before kernel instantiation and ends when the host sees PID 1’s first output marker. Node startup, downloads, disk preparation and initial kernel module compilation happened before timing. The other rows use the guest monotonic clock. Syscall, file and process timings include the behavior of the respective Spore and Linux kernels.

Spore used the ARM64 trace JIT with a 256-visit compilation threshold. Before trace compilation, the same Spore workload took 2,898 ms at commit `9646419`. The reference was the published `unkernel@0.2.0-261125-cec0a49` package, which includes the Zig kwemu userspace CPU. [Reference source](https://github.com/pithings/unkernel/tree/cec0a49)

## Reproduce

From the development shell, run

```sh
cargo build --release
./target/release/spore bench kwemu
```

The benchmark fetches pinned guest and reference artifacts and prints every sample. For an offline rerun with the build dependencies cached, set `UNKERNEL_PACKAGE_DIR` to the extracted npm package and `BUSYBOX_ARM64` to the matching static executable.
