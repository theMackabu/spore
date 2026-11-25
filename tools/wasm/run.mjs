// The host supplies byte streams and a read-only block device. The kernel owns paths and FDs.
export async function run(
  kernel,
  elf,
  { args = [], argv0 = "/app/server", readDisk, write, instructionLimit = 100_000_000, signal } = {},
) {
  if (!(elf instanceof Uint8Array) || !Number.isSafeInteger(instructionLimit) || instructionLimit < 1) {
    throw new TypeError("expected ELF bytes and a positive instruction limit");
  }
  if (
    args.length >= 128 ||
    [argv0, ...args].some((arg) => typeof arg !== "string" || arg.includes("\0"))
  ) {
    throw new Error("expected at most 127 NUL-free guest arguments");
  }
  const bundled = new TextDecoder().decode(elf.subarray(0, 8)) === "SPOREAP2";
  if (bundled && args.length) throw new Error("packaged applications already contain their arguments");
  const argv = bundled
    ? new Uint8Array()
    : new TextEncoder().encode([argv0, ...args].map((arg) => arg + "\0").join(""));
  const decoder = new TextDecoder();
  let guest;
  const bytes = (ptr, len) => new Uint8Array(guest.memory.buffer, ptr >>> 0, len >>> 0);
  const stop = {};
  let status;
  const module = kernel instanceof WebAssembly.Module ? kernel : await WebAssembly.compile(kernel);
  guest = (
    await WebAssembly.instantiate(module, {
      spore: {
        disk_read(offset, ptr, len) {
          return readDisk?.(offset, bytes(ptr, len)) === len ? 1 : 0;
        },
        write(fd, ptr, len) {
          write?.(fd, bytes(ptr, len).slice());
        },
        clock() {
          return BigInt(Math.floor(performance.now() * 1000));
        },
        random(ptr, len) {
          crypto.getRandomValues(bytes(ptr, len));
        },
        finish(kind, code) {
          if (kind !== 69) throw new Error(`guest ${kind === 83 ? "signal" : "boot error"} ${code}`);
          status = code & 255;
          throw stop;
        },
        fault(pc, ptr, len) {
          throw new Error(`${decoder.decode(bytes(ptr, len))} (PC 0x${pc.toString(16)})`);
        },
      },
    })
  ).exports;
  const size = elf.byteLength + argv.byteLength;
  if (size > 128 * 1024 * 1024) throw new Error("input exceeds staging memory");
  const ptr = guest.stage(size);
  if (!ptr) throw new Error("cannot stage guest input");
  bytes(ptr, elf.length).set(elf);
  bytes(ptr + elf.length, argv.length).set(argv);
  try {
    if (!guest.start(elf.length, !!readDisk, BigInt(Math.floor(Date.now() / 1000)))) {
      throw new Error("invalid ELF input");
    }
    let remaining = instructionLimit;
    let nextTick = performance.now() + 10;
    let yieldAt = performance.now() + 8;
    while (remaining > 0) {
      signal?.throwIfAborted();
      const count = Math.min(remaining, 100_000);
      const now = performance.now();
      const ticks = Math.min(64, Math.max(0, Math.floor((now - nextTick) / 10) + 1));
      nextTick += ticks * 10;
      const retired = guest.run_slice(count, ticks);
      if (retired < 0) throw new Error("CPU stopped without a guest result");
      remaining -= retired;
      if (retired < count || (remaining && performance.now() >= yieldAt)) {
        const delay = retired < count ? Math.max(0, nextTick - performance.now()) : 0;
        await new Promise((resolve) => setTimeout(resolve, delay));
        yieldAt = performance.now() + 8;
      }
    }
    throw new Error("guest instruction limit exceeded");
  } catch (error) {
    if (error !== stop) throw error;
    return status;
  }
}
