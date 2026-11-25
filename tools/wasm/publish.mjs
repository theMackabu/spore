import { createHash, randomUUID } from "node:crypto";
import { open, mkdir, readFile, writeFile, rename, rm, realpath, access } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { bootPages } from "./boot-pages.mjs";

export const assets = [
  "index.html",
  "terminal.mjs",
  "worker.mjs",
  "run.mjs",
  "disk.mjs",
  "input.mjs",
  "browser-disk.mjs",
  "persistent.mjs",
  "release-disk.mjs",
  "spore_wasm.wasm",
  "upgrade.wasm",
  "xterm/xterm.mjs",
  "xterm/xterm.css",
  "fit/addon-fit.mjs",
  "xterm/LICENSE",
  "fit/LICENSE",
];
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
async function atomic(path, bytes) {
  const tmp = path + "." + randomUUID();
  try {
    await writeFile(tmp, bytes);
    await rename(tmp, path);
  } finally {
    await rm(tmp, { force: true });
  }
}

async function diskImage(file, size, out) {
  const chunkSize = 256 * 1024;
  const buf = Buffer.alloc(chunkSize),
    zero = Buffer.alloc(chunkSize);
  const chunks = {};
  for (let offset = 0; offset < size;) {
    const length = Math.min(chunkSize, size - offset);
    let done = 0;
    while (done < length) {
      const { bytesRead } = await file.read(buf, done, length - done, offset + done);
      if (!bytesRead) throw Error("Root image changed during publication");
      done += bytesRead;
    }
    const bytes = buf.subarray(0, length);
    if (!bytes.equals(zero.subarray(0, length))) {
      const id = hash(bytes);
      const path = join(out, "objects", id);
      try {
        await access(path);
      } catch {
        await atomic(path, bytes);
      }
      chunks[offset / chunkSize] = id;
    }
    offset += length;
  }
  const json = JSON.stringify({ size, chunkSize, chunks });
  const id = hash(json);
  await atomic(join(out, "disks", id + ".json"), json);
  return id;
}

export async function publish(build, out, reuseDisk = false) {
  const web = join(build, "wasm-web");
  await Promise.all(
    ["objects", "disks", "releases"].map((dir) => mkdir(join(out, dir), { recursive: true })),
  );
  const disk = await open(join(build, "root.ext2"), "r");
  try {
    const info = await disk.stat();
    const st = await disk.stat({ bigint: true });
    const stamp = [st.dev, st.ino, st.size, st.mtimeNs, st.ctimeNs].join(":");
    const cache = join(build, ".wasm-published-disk.json");
    let diskId;
    if (reuseDisk) {
      try {
        const saved = JSON.parse(await readFile(cache, "utf8"));
        if (saved.stamp === stamp && /^[a-f0-9]{64}$/.test(saved.id)) {
          await access(join(out, "disks", saved.id + ".json"));
          diskId = saved.id;
        }
      } catch {
        /* A missing local cache is rebuilt from the image. */
      }
    }
    if (!diskId) {
      diskId = await diskImage(disk, info.size, out);
      if (reuseDisk) await atomic(cache, JSON.stringify({ stamp, id: diskId }));
    }
    const files = await Promise.all(assets.map(async (name) => [name, await readFile(join(web, name))]));
    const id = hash(JSON.stringify([diskId, files.map(([name, bytes]) => [name, hash(bytes)])]));
    const dest = join(out, "releases", id);
    const stage = dest + "." + randomUUID();
    try {
      await access(dest);
    } catch {
      try {
        for (const [name, bytes] of files) {
          const path = join(stage, name);
          await mkdir(dirname(path), { recursive: true });
          await writeFile(path, bytes);
        }
        const warm = await bootPages(disk.fd, info.size, join(web, "spore_wasm.wasm"));
        await writeFile(join(stage, "boot.pages.gz"), warm);
        try {
          await rename(stage, dest);
        } catch (e) {
          if (!["ENOTEMPTY", "EEXIST"].includes(e.code)) throw e;
        }
      } finally {
        await rm(stage, { force: true, recursive: true });
      }
    }
    await atomic(join(out, "index.html"), files[0][1]);
    await atomic(
      join(out, "release.json"),
      JSON.stringify({ format: 1, id, disk: diskId, modified: info.mtime.toUTCString() }) + "\n",
    );
    return id;
  } finally {
    await disk.close();
  }
}
if (process.argv[1] && (await realpath(process.argv[1])) === fileURLToPath(import.meta.url)) {
  const [, , build, out] = process.argv;
  console.log("Published", await publish(build, out), "to", out);
}
