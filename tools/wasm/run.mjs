export class GuestSignal extends Error {
  constructor(signal, detail) {
    super(`guest signal ${signal}${detail ? " (" + detail + ")" : ""}`);
    this.signal = signal;
  }
}

export async function run(
  kernel,
  elf,
  { args = [], argv0 = "/app/server", readDisk, writeDisk, readStdin, readByte, winsize, network, write, onException, memoryMiB = 64, cpuCount = 1, jit = true, stats, instructionLimit = 100_000_000, signal } = {},
) {
  if (!Number.isInteger(memoryMiB) || memoryMiB < 16 || memoryMiB >= 2048) {
    throw new RangeError("Wasm guest RAM must be 16 through 2047 MiB");
  }
  if (!Number.isInteger(cpuCount) || cpuCount < 1 || cpuCount > 64) throw new RangeError("expected 1 through 64 guest CPUs");
  const system = elf === null;
  if (system && (!readDisk || args.length)) throw new Error("system boot requires a root disk");
  if ((!system && !(elf instanceof Uint8Array)) || !Number.isSafeInteger(instructionLimit) || instructionLimit < 1) {
    throw new TypeError("expected ELF bytes and a positive instruction limit");
  }
  if (
    args.length >= 128 ||
    [argv0, ...args].some((arg) => typeof arg !== "string" || arg.includes("\0"))
  ) {
    throw new Error("expected at most 127 NUL-free guest arguments");
  }
  const bundled = !system && new TextDecoder().decode(elf.subarray(0, 8)) === "SPOREAP2";
  if (bundled && args.length) throw new Error("packaged applications already contain their arguments");
  const argv = bundled || system
    ? new Uint8Array()
    : new TextEncoder().encode([argv0, ...args].map((arg) => arg + "\0").join(""));
  const decoder = new TextDecoder();
  let guest;
  const blocks = [];
  if (stats) Object.assign(stats, { compiledBlocks: 0, compiledInstructions: 0 });
  const bytes = (ptr, len) => new Uint8Array(guest.memory.buffer, ptr >>> 0, len >>> 0);
  const stop = {};
  let status;
  const module = kernel instanceof WebAssembly.Module ? kernel : await WebAssembly.compile(kernel);
  guest = (
    await WebAssembly.instantiate(module, {
      spore: {
        jit_compile(slot, ptr, len) {
          try {
            const code = new WebAssembly.Module(bytes(ptr, len));
            blocks[slot] = new WebAssembly.Instance(code, { spore: { memory: guest.memory } }).exports.run;
            if (stats) ++stats.compiledBlocks;
            return 1;
          } catch (e) {
            if (e instanceof WebAssembly.CompileError) return 0;
            throw e;
          }
        },
        jit_run(slot, frame, budget) {
          const retired = blocks[slot](frame, budget);
          if (stats) stats.compiledInstructions += retired;
          return retired;
        },
        net_send(ptr, len) {
          return network?.send(bytes(ptr, len).slice()) ? 1 : 0;
        },
        net_recv(ptr, len) {
          const frame = network?.receive();
          if (!frame || frame.length > len) return 0;
          bytes(ptr, frame.length).set(frame);
          return frame.length;
        },
        exception(pc, ptr, len) {
          onException?.(pc, decoder.decode(bytes(ptr, len)));
        },
        winsize() {
          const {rows = 24, cols = 80} = winsize?.() ?? {};
          return ((rows & 0xffff) << 16) | (cols & 0xffff);
        },
        stdin_read(ptr, len) {
          return readStdin?.(bytes(ptr, len)) ?? -1;
        },
        serial_read() {
          return readByte?.() ?? -1;
        },
        disk_write(offset, ptr, len) {
          return writeDisk?.(offset, bytes(ptr, len)) === len ? 1 : 0;
        },
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
        finish(kind, code, pc, ptr, len) {
          if (kind === 83) {
            const detail = len ? `${decoder.decode(bytes(ptr, len))} at PC 0x${pc.toString(16)}` : "";
            throw new GuestSignal(code, detail);
          }
          if (kind !== 69) throw new Error(`guest boot error ${code}`);
          status = code & 255;
          throw stop;
        },
      },
    })
  ).exports;
  const size = (elf?.byteLength ?? 0) + argv.byteLength;
  if (!system) {
    if (size > 0x7fffffff) throw new Error("input exceeds wasm32 allocation size");
    const ptr = guest.stage(size);
    if (!ptr) throw new Error("cannot stage guest input");
    bytes(ptr, elf.length).set(elf);
    bytes(ptr + elf.length, argv.length).set(argv);
  }
  const devices = (readDisk ? 1 : 0) | (writeDisk ? 2 : 0) | (readByte ? 4 : 0) | (network ? 8 : 0) | (jit ? 16 : 0) | (readStdin ? 32 : 0);
  try {
    if (!guest.start(elf?.length ?? 0, devices, BigInt(Math.floor(Date.now() / 1000)), memoryMiB, cpuCount)) {
      throw new Error("cannot initialize guest memory or executable");
    }
    let remaining = instructionLimit;
    let nextTick = performance.now() + 10;
    let yieldAt = performance.now() + 8;
    while (remaining > 0) {
      signal?.throwIfAborted();
      const count = Math.min(remaining, 100_000);
      const now = performance.now();
      const ticks = Number(now >= nextTick);
      if (ticks) nextTick = now + 10;
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
