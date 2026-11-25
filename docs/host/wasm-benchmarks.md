# Spore and kwemu

Spore reached the first userspace write sooner in this run. kwemu finished the BusyBox pipeline sooner. Both produced the same checksums and child statuses.

| Workload | Spore, ms | unkernel with kwemu, ms |
|---|---:|---:|
| Fresh kernel instance to first PID 1 write | 7.37 (6.36–9.51) | 17.13 (15.21–19.05) |
| 16,777,216 ARM64 integer loop instructions | 1.90 (1.61–2.66) | 1.58 (1.53–1.62) |
| 100,000 calls through musl’s generic `syscall(SYS_getpid)` wrapper | 28.05 (25.48–30.88) | 9.30 (8.76–11.46) |
| 100,000 direct `svc` calls to `getpid` | 6.36 (6.07–7.35) | 4.93 (4.64–6.41) |
| 100,000 cached 64-byte `pread` calls on tmpfs | 43.43 (41.77–45.73) | 15.89 (15.62–17.69) |
| 128 MiB copied with musl `memcpy` | 23.69 (21.71–24.56) | 10.83 (9.40–12.52) |
| 200,000 ARM SHA256H/SHA256H2 instruction pairs | 11.35 (10.43–13.77) | 3.20 (3.03–3.31) |
| 200 fork, child exit and parent wait cycles | 4.96 (3.75–7.21) | 4.71 (4.48–6.57) |
| BusyBox reverse-sort and SHA-256 pipeline, 25,000 lines | 2,898.01 (2,823.85–3,132.81) | 381.14 (378.13–401.26) |

Values are medians, with the observed minimum and maximum in parentheses. The runner discarded one warm-up per engine, then alternated engine order over five measured runs each. Fork timings overlapped across runs.

## What was measured

The host was an Apple M5 Pro with 64 GB of RAM, macOS 27.0.1 and Node 24.15.0. Each engine booted a fresh copy of the same 16 MiB ext2 disk from host RAM with one virtual CPU, a 128 MiB memory setting, networking disabled and its JIT enabled. The disk contained one static AArch64 musl PID 1 built with GCC 15.2.0 and Alpine’s AArch64 BusyBox 1.37.0-r30. The guest ran `seq 1 25000 | sort -r | sha256sum` with all three applets from that BusyBox build. The runner checked its digest against an independent host sort. [Guest workload](../../tests/wasm/workload.c) · [Runner](../../tests/wasm/compare.mjs)

Boot timing starts immediately before kernel instantiation and ends when the host sees PID 1’s first output marker. Node startup, downloads, disk preparation and initial kernel module compilation happened before timing. The other rows use the guest monotonic clock. Syscall, file and process timings include the behavior of the respective Spore and Linux kernels.

Spore was measured at commit `9646419`. The reference was the published `unkernel@0.2.0-261125-cec0a49` package, which includes the Zig kwemu userspace CPU. [Reference source](https://github.com/pithings/unkernel/tree/cec0a49)

## Reproduce

From the development shell, run

```sh
cargo build --release
./target/release/spore bench kwemu
```

The benchmark fetches pinned guest and reference artifacts and prints every sample. For an offline rerun with the build dependencies cached, set `UNKERNEL_PACKAGE_DIR` to the extracted npm package and `BUSYBOX_ARM64` to the matching static executable.
