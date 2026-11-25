import fs from "node:fs";
import { snapshotDisk } from "./disk.mjs";
import { userNetwork } from "./net.mjs";
import { SerialInput } from "./input.mjs";
import { GuestSignal, run } from "./run.mjs";

const [kernel, mode, path, ram, cpus, data, qemu, settings] = process.argv.slice(2);
const system = mode === "boot";
const diskPath = system ? path : data || undefined;
const terminal = process.stdin.isTTY;
const serial = new SerialInput();
const stop = new AbortController();
const raw = process.stdin.isRaw;
let disk, network, interrupted;
function interrupt(signal) {
  interrupted = signal === "SIGTERM" ? 143 : 130;
  stop.abort(new Error("interrupted"));
}
process.on("SIGTERM", interrupt);
process.on("SIGINT", interrupt);
function receive(bytes) {
  if (terminal && bytes.includes(0x1d)) stop.abort(new Error("console disconnected"));
  else serial.push(bytes);
}
try {
  const match = /^(\d+)([MG]?)$/i.exec(ram);
  if (!match) throw new Error("expected guest RAM in MiB or GiB (e.g. 128M)");
  const memoryMiB = Number(match[1]) * (match[2].toUpperCase() === "G" ? 1024 : 1);
  if (settings) network = await userNetwork(qemu, settings, stop);
  let media;
  if (diskPath !== undefined) {
    disk = fs.openSync(diskPath, system ? "r" : "r+");
    const readDisk = (offset, dest) => fs.readSync(disk, dest, 0, dest.length, offset);
    media = system ? snapshotDisk(fs.fstatSync(disk).size, readDisk) : { readDisk, writeDisk: (offset, data) => fs.writeSync(disk, data, 0, data.length, Number(offset)) };
  }
  if (terminal || system) {
    if (terminal) process.stdin.setRawMode(true);
    process.stdin.on("data", receive);
  }
  process.exitCode = await run(fs.readFileSync(kernel), system ? null : fs.readFileSync(path), {
    ...media,
    memoryMiB,
    cpuCount: Number(cpus),
    network,
    onException: process.env.SPORE_WASM_TRACE ? (pc, msg) => console.error(`[wasm] ${msg} at PC 0x${pc.toString(16)}`) : undefined,
    readByte: terminal || system ? serial.readByte : undefined,
    instructionLimit: system || network ? Number.MAX_SAFE_INTEGER : undefined,
    signal: stop.signal,
    write(fd, data) {
      for (let i = 0; i < data.length;) i += fs.writeSync(fd, data, i, data.length - i);
    },
  });
} catch (e) {
  console.error("spore:", e.message);
  process.exitCode = interrupted ?? (e instanceof GuestSignal ? 128 + e.signal : 125);
} finally {
  network?.close();
  process.off("SIGINT", interrupt);
  process.off("SIGTERM", interrupt);
  process.stdin.off("data", receive);
  process.stdin.pause();
  if (terminal) process.stdin.setRawMode(!!raw);
  if (disk !== undefined) fs.closeSync(disk);
}
