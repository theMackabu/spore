import fs from "node:fs";
import { snapshotDisk } from "./disk.mjs";
import { SerialInput } from "./input.mjs";
import { GuestSignal, run } from "./run.mjs";

const [kernel, mode, path, data] = process.argv.slice(2);
const system = mode === "boot";
const diskPath = system ? path : data;
const terminal = process.stdin.isTTY;
const serial = new SerialInput();
const stop = new AbortController();
const raw = process.stdin.isRaw;
let disk;
function receive(bytes) {
  if (terminal && bytes.includes(0x1d)) stop.abort(new Error("console disconnected"));
  else serial.push(bytes);
}
try {
  let media;
  if (diskPath !== undefined) {
    disk = fs.openSync(diskPath, "r");
    const readDisk = (offset, dest) => fs.readSync(disk, dest, 0, dest.length, offset);
    media = system ? snapshotDisk(fs.fstatSync(disk).size, readDisk) : { readDisk };
  }
  if (terminal || system) {
    if (terminal) process.stdin.setRawMode(true);
    process.stdin.on("data", receive);
  }
  process.exitCode = await run(fs.readFileSync(kernel), system ? null : fs.readFileSync(path), {
    ...media,
    onException: process.env.SPORE_WASM_TRACE ? (pc, msg) => console.error(`[wasm] ${msg} at PC 0x${pc.toString(16)}`) : undefined,
    readByte: terminal || system ? serial.readByte : undefined,
    instructionLimit: system ? Number.MAX_SAFE_INTEGER : undefined,
    signal: stop.signal,
    write(fd, data) {
      for (let i = 0; i < data.length;) i += fs.writeSync(fd, data, i, data.length - i);
    },
  });
} catch (e) {
  console.error("spore:", e.message);
  process.exitCode = e instanceof GuestSignal ? 128 + e.signal : 125;
} finally {
  process.stdin.off("data", receive);
  process.stdin.pause();
  if (terminal) process.stdin.setRawMode(!!raw);
  if (disk !== undefined) fs.closeSync(disk);
}
