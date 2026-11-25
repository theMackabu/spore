import { execFileSync } from "node:child_process";
import fs from "node:fs";
import { createHash } from "node:crypto";
import os from "node:os";
import path from "node:path";
import { pathToFileURL, fileURLToPath } from "node:url";
import { run } from "../../tools/wasm/run.mjs";

const version = "0.2.0-261125-cec0a49";
const [modulePath, referenceDir] = process.argv.slice(2);
const temp = fs.mkdtempSync(path.join(os.tmpdir(), "spore-kwemu-"));
const exec = (cmd, args, options = {}) =>
  execFileSync(cmd, args, { stdio: ["ignore", "pipe", "inherit"], ...options });
try {
  let pkg = referenceDir;
  if (!pkg) {
    const tar = exec("npm", ["pack", "--ignore-scripts", "--silent", "unkernel@" + version], { cwd: temp })
      .toString()
      .trim();
    exec("bsdtar", ["-xf", tar], { cwd: temp });
    pkg = path.join(temp, "package");
  }
  if (JSON.parse(fs.readFileSync(path.join(pkg, "package.json"))).version !== version)
    throw Error("unexpected unkernel version");
  const { createKernel } = await import(pathToFileURL(path.resolve(pkg, "dist/node.mjs")));
  const root = path.join(temp, "root"),
    disk = path.join(temp, "bench.ext2");
  for (const name of ["sbin", "dev", "tmp", "proc", "sys", "bin"])
    fs.mkdirSync(path.join(root, name), { recursive: true });
  const busybox = path.join(root, "bin/busybox");
  if (process.env.BUSYBOX_ARM64) fs.copyFileSync(process.env.BUSYBOX_ARM64, busybox);
  else {
    const apk = path.join(temp, "busybox.apk");
    exec("curl", [
      "-fsSL",
      "--retry",
      "3",
      "-o",
      apk,
      "https://dl-cdn.alpinelinux.org/alpine/v3.23/main/aarch64/busybox-static-1.37.0-r30.apk",
    ]);
    if (
      createHash("sha256").update(fs.readFileSync(apk)).digest("hex") !==
      "44c9abdfb970f398fa72c8382fe2d8808eea16beaf82daf6ac708b92f1b8659e"
    )
      throw Error("BusyBox archive checksum mismatch");
    fs.writeFileSync(busybox, exec("bsdtar", ["-xOf", apk, "bin/busybox.static"], { maxBuffer: 4 << 20 }));
  }
  if (
    createHash("sha256").update(fs.readFileSync(busybox)).digest("hex") !==
    "198f6f675a49ac734082e054b823d3e2e947180ca2f632852ae1c09e950fd676"
  )
    throw Error("unexpected BusyBox executable");
  fs.chmodSync(busybox, 0o755);
  fs.symlinkSync("busybox", path.join(root, "bin/sh"));
  const sorted =
    Array.from({ length: 25000 }, (_, i) => String(i + 1))
      .sort()
      .reverse()
      .join("\n") + "\n";
  const sortHash = createHash("sha256").update(sorted).digest("hex");
  const source = fileURLToPath(new URL("./workload.c", import.meta.url));
  exec("aarch64-unknown-linux-musl-gcc", [
    "-static",
    "-O2",
    "-march=armv8-a+crypto",
    source,
    "-o",
    path.join(root, "sbin/init"),
  ]);
  exec("mke2fs", ["-q", "-t", "ext2", "-b", "4096", "-O", "none,filetype", "-d", root, disk, "4096"]);
  exec("debugfs", ["-w", "-f", "-", disk], { input: "cd /dev\nmknod console c 5 1\n" });
  const diskBytes = fs.readFileSync(disk);
  const spore = await WebAssembly.compile(fs.readFileSync(modulePath));
  const kwemu = await WebAssembly.compile(fs.readFileSync(path.join(pkg, "dist/unkernel.wasm")));
  const all = { spore: [], kwemu: [] };
  let checks;
  for (let round = 0; round < 6; round++) {
    for (const engine of round % 2 ? ["kwemu", "spore"] : ["spore", "kwemu"]) {
      const image = Buffer.from(diskBytes);
      let log = "",
        entry;
      const output = (bytes) => {
        log += Buffer.from(bytes).toString();
        if (entry === undefined && log.includes("SPORE_BENCH_ENTRY")) entry = performance.now() - start;
      };
      const options = { memoryMiB: 128, jit: true };
      const signal = AbortSignal.timeout(90000);
      const start = performance.now();
      if (engine === "spore") {
        const status = await run(spore, null, {
          ...options,
          signal,
          readDisk(off, d) {
            d.set(image.subarray(Number(off), Number(off) + d.length));
            return d.length;
          },
          writeDisk(off, d) {
            image.set(d, Number(off));
            return d.length;
          },
          write: (_, bytes) => output(bytes),
          instructionLimit: Number.MAX_SAFE_INTEGER,
        });
        if (status !== 0) throw Error("Spore exited " + status + "\n" + log);
      } else {
        const vm = await createKernel({
          ...options,
          kernel: kwemu,
          cpus: 1,
          worker: false,
          initrd: null,
          cmdline: "console=hvc0 root=/dev/wda rootfstype=ext4 rw init=/sbin/init quiet loglevel=0",
          signal,
          disks: [
            {
              size: image.length,
              read(off, views) {
                for (const v of views) {
                  v.set(image.subarray(off, off + v.length));
                  off += v.length;
                }
              },
              write(off, views) {
                for (const v of views) {
                  image.set(v, off);
                  off += v.length;
                }
              },
            },
          ],
          onOutput: output,
          log: () => {},
        });
        const exit = await vm.exited;
        if (exit.code !== 0) throw Error("unkernel exited " + JSON.stringify(exit) + "\n" + log);
      }
      if (!entry || !log.includes("SPORE_BENCH_DONE"))
        throw Error("incomplete " + engine + " guest output\n" + log);
      const row = { boot_ms: entry },
        sums = {};
      for (const match of log.matchAll(/SAMPLE (\w+) (\d+) (\d+)/g)) {
        row[match[1]] = Number(match[2]) / 1e6;
        sums[match[1]] = match[3];
      }
      if (
        Object.keys(sums).length !== 8 ||
        !log.includes("SORT_HASH " + sortHash) ||
        sums.getpid !== "100000" ||
        sums.getpid_raw !== "100000" ||
        sums.fork_wait !== "8400"
      )
        throw Error("invalid benchmark results " + log);
      if (checks && JSON.stringify(checks) !== JSON.stringify(sums)) throw Error("guest checksums differ");
      checks = sums;
      if (round) all[engine].push(row);
      console.log(engine, round, JSON.stringify(row));
    }
  }
  for (const key of Object.keys(all.spore[0])) {
    const median = (rows) => rows.map((row) => row[key]).toSorted((a, b) => a - b)[2];
    console.log(
      key,
      "Spore",
      median(all.spore).toFixed(3),
      "ms; kwemu",
      median(all.kwemu).toFixed(3),
      "ms",
    );
  }
} finally {
  fs.rmSync(temp, { recursive: true, force: true });
}
