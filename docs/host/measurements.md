# EFI app boot measurements

One Apple Silicon sweep used macOS 27.0.1, QEMU 11.1.3 with HVF, one vCPU, a 15 MiB
Ant image and fresh firmware variables for each cold boot.

| Guest RAM | First instruction fetch | First host HTTP response | Steady Ant RSS | Guest RAM in use |
| --- | --- | --- | --- | --- |
| 32 MiB | Firmware stopped | — | — | — |
| 64 MiB | Firmware stopped | — | — | — |
| 128 MiB | 174 ms | 387 ms | 4,812 KiB | 55,404 KiB |

The bundled EDK2 firmware enforced a 128 MiB minimum before loading Limine. Entry time
runs from QEMU launch until the host observes the guest resolving its first user instruction
fetch. HTTP time starts at CLI launch. The harness sampled memory half a second after five
successful requests. Guest RAM in use includes firmware and payload pages.

Run `./target/release/spore bench appliance` to repeat the sweep. [Measurement harness](../../tests/appliance/check.py) · [App results](results.md)
