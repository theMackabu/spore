import fs from "node:fs";
import { GuestSignal, run } from "./run.mjs";

const [kernel, payload, diskPath] = process.argv.slice(2);
let disk;
try {
  if (diskPath !== undefined) disk = fs.openSync(diskPath, "r");
  process.exitCode = await run(fs.readFileSync(kernel), fs.readFileSync(payload), {
    readDisk:
      disk === undefined ? undefined : (offset, dest) => fs.readSync(disk, dest, 0, dest.length, offset),
    write(fd, data) {
      for (let i = 0; i < data.length;) i += fs.writeSync(fd, data, i, data.length - i);
    },
  });
} catch (e) {
  console.error("spore:", e.message);
  process.exitCode = e instanceof GuestSignal ? 128 + e.signal : 125;
} finally {
  if (disk !== undefined) fs.closeSync(disk);
}
