import fs from "node:fs";
import { parseArgs } from "node:util";
import { snapshotDisk } from "./disk.mjs";
import { userNetwork } from "./net.mjs";
import { SerialInput } from "./input.mjs";
import { GuestBootError, GuestSignal, run } from "./run.mjs";

const [kernel, mode, path, ...args] = process.argv.slice(2);
const { values: opt } = parseArgs({
  args,
  options: {
    memory: { type: "string", default: "64M" },
    cpus: { type: "string", default: "1" },
    disk: { type: "string" },
    net: { type: "string" },
    qemu: { type: "string" },
    result: { type: "string" },
    "boot-timeout": { type: "string" },
    "boot-image": { type: "string" },
    writable: { type: "boolean" },
  },
});
const system = mode === "system";
const diskPath = system ? path : opt.disk;
const ram = opt.memory,
  cpus = opt.cpus;
const started = performance.now();
let resultKind = "host-error",
  resultValue = 125,
  entry = null;
const terminal = process.stdin.isTTY;
const serial = new SerialInput();
const stop = new AbortController();
const raw = process.stdin.isRaw;
let disk, bootImage, network, interrupted;
function interrupt(signal) {
  interrupted = signal === "SIGTERM" ? 143 : 130;
  stop.abort(new Error("interrupted"));
}
process.on("SIGTERM", interrupt);
process.on("SIGINT", interrupt);
const piped = !terminal && !system;
function readable() {}
function receive(bytes) {
  if (terminal && bytes.includes(0x1d)) stop.abort(new Error("console disconnected"));
  else serial.push(bytes);
}
try {
  const match = /^(\d+)([MG]?)$/i.exec(ram);
  if (!match) throw new Error("expected guest RAM in MiB or GiB (e.g. 128M)");
  const memoryMiB = Number(match[1]) * (match[2].toUpperCase() === "G" ? 1024 : 1);
  if (opt.net) network = await userNetwork(opt.qemu, opt.net, stop);
  if (opt["boot-image"]) bootImage = fs.openSync(opt["boot-image"], "r");
  let media;
  if (diskPath !== undefined) {
    const snapshot = system && !opt.writable;
    disk = fs.openSync(diskPath, snapshot ? "r" : "r+");
    const readDisk = (offset, dest) => fs.readSync(disk, dest, 0, dest.length, offset);
    media = snapshot
      ? snapshotDisk(fs.fstatSync(disk).size, readDisk)
      : {
          readDisk,
          writeDisk: (offset, data) => fs.writeSync(disk, data, 0, data.length, Number(offset)),
          flushDisk: () => fs.fsyncSync(disk),
        };
  }
  if (piped) process.stdin.on("readable", readable);
  if (terminal || system) {
    if (terminal) process.stdin.setRawMode(true);
    process.stdin.on("data", receive);
  }
  resultKind = "vm-error";
  process.exitCode = await run(fs.readFileSync(kernel), system ? null : fs.readFileSync(path), {
    ...media,
    readBoot:
      bootImage === undefined
        ? undefined
        : (offset, dest) => fs.readSync(bootImage, dest, 0, dest.length, offset),
    onEntry() {
      entry = Math.round((performance.now() - started) * 1000);
      if (!system && opt["boot-timeout"] && entry > Number(opt["boot-timeout"]) * 1e6) {
        resultKind = "boot-timeout";
        entry = null;
        throw new Error("guest boot timed out");
      }
    },
    memoryMiB,
    cpuCount: Number(cpus),
    network,
    kernelLog: process.env.SPORE_WASM_KLOG ? (bytes) => process.stderr.write(bytes) : undefined,
    onException: process.env.SPORE_WASM_TRACE
      ? (pc, msg) => console.error(`[wasm] ${msg} at PC 0x${pc.toString(16)}`)
      : undefined,
    winsize: () => ({ rows: process.stdout.rows, cols: process.stdout.columns }),
    readStdin: piped
      ? (dest) => {
          const n = Math.min(process.stdin.readableLength, dest.length);
          const data = process.stdin.read(n);
          if (!data) return process.stdin.readableEnded || process.stdin.destroyed ? -1 : 0;
          dest.set(data);
          return data.length;
        }
      : undefined,
    readByte: terminal || system ? serial.readByte : undefined,
    instructionLimit: Number.MAX_SAFE_INTEGER,
    signal: stop.signal,
    write(fd, data) {
      for (let i = 0; i < data.length;) i += fs.writeSync(fd, data, i, data.length - i);
    },
  });
  resultKind = "exit";
  resultValue = process.exitCode;
} catch (e) {
  if (e instanceof GuestBootError) {
    resultKind = "boot-error";
    resultValue = e.code;
  }
  if (e instanceof GuestSignal) {
    resultKind = "signal";
    resultValue = e.signal;
  }
  if (interrupted) {
    resultKind = "host-signal";
    resultValue = interrupted - 128;
  }
  console.error("spore:", e.message);
  process.exitCode = interrupted ?? (e instanceof GuestSignal ? 128 + e.signal : 125);
} finally {
  network?.close();
  process.off("SIGINT", interrupt);
  process.off("SIGTERM", interrupt);
  process.stdin.off("data", receive);
  process.stdin.off("readable", readable);
  process.stdin.pause();
  if (terminal) process.stdin.setRawMode(!!raw);
  if (disk !== undefined) fs.closeSync(disk);
  if (bootImage !== undefined) fs.closeSync(bootImage);
  if (opt.result) {
    try {
      fs.writeFileSync(
        opt.result,
        JSON.stringify({ kind: resultKind, value: resultValue, entry_us: entry }) + "\n",
      );
    } catch (e) {
      console.error("spore:", e.message);
      process.exitCode = 125;
    }
  }
}
