import { snapshotDisk } from "./disk.mjs";

const PAGE = 4096;
async function openFile(dir, name) {
  const file = await dir.getFileHandle(name, { create: true });
  return file.createSyncAccessHandle();
}

async function openDiskFiles(name, { signal, onWaiting } = {}) {
  let delay = 50;
  let notified = false;
  for (;;) {
    signal?.throwIfAborted();
    let index;
    try {
      const root = await navigator.storage.getDirectory();
      const dir = await root.getDirectoryHandle(name, { create: true });
      index = await openFile(dir, "index");
      const pages = await openFile(dir, "pages");
      return { index, pages };
    } catch (error) {
      index?.close();
      if (error.name !== "NoModificationAllowedError" && error.name !== "NotFoundError") throw error;
      if (!notified && delay >= 400) {
        onWaiting?.(error);
        notified = true;
      }
      await new Promise((resolve) => setTimeout(resolve, delay));
      delay = Math.min(delay * 2, 1000);
    }
  }
}

export async function diskKey(file, read) {
  const header = new Uint8Array(Math.min(65536, file.size));
  if (read(0n, header) !== header.length) throw new Error("Cannot identify the root disk");
  const identity = new TextEncoder().encode(JSON.stringify([file.name, file.size, file.lastModified]));
  const bytes = new Uint8Array(identity.length + header.length);
  bytes.set(identity);
  bytes.set(header, identity.length);
  const hash = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  const key = Array.from(hash, (b) => b.toString(16).padStart(2, "0")).join("");
  return "spore-" + key;
}

export async function persistentDisk(file, read, options) {
  return openOverlay(await diskKey(file, read), file.size, read, options);
}

export async function openOverlay(name, size, read, options) {
  const { index, pages } = await openDiskFiles(name, options);
  try {
    options?.signal?.throwIfAborted();
    if (options?.resetDisk) {
      index.truncate(0);
      index.flush();
      pages.truncate(0);
      pages.flush();
    }
    const records = new Map();
    let dirty = false;
    const entries = index.getSize() / 8;
    if (!Number.isInteger(entries) || entries > Math.ceil(size / PAGE) || pages.getSize() < entries * PAGE)
      throw new Error("Incomplete persistent disk data");
    const batch = new Uint8Array(8192);
    const view = new DataView(batch.buffer);
    for (let start = 0; start < entries;) {
      const n = Math.min(batch.length / 8, entries - start);
      if (index.read(batch.subarray(0, n * 8), { at: start * 8 }) !== n * 8)
        throw new Error("Cannot read disk index");
      for (let i = 0; i < n; i++) {
        const base = Number(view.getBigUint64(i * 8, true));
        if (!Number.isSafeInteger(base) || base % PAGE || base >= size || records.has(base)) {
          throw new Error("Invalid persistent disk index");
        }
        records.set(base, start + i);
      }
      start += n;
    }
    const overlay = snapshotDisk(size, read, {
      get(base) {
        const slot = records.get(base);
        if (slot === undefined) return undefined;
        const buf = new Uint8Array(Math.min(PAGE, size - base));
        if (pages.read(buf, { at: slot * PAGE }) !== buf.length)
          throw new Error("Cannot read saved disk page");
        return buf;
      },
      set(base, buf) {
        dirty = true;
        const exists = records.has(base);
        const slot = exists ? records.get(base) : records.size;
        if (pages.write(buf, { at: slot * PAGE }) !== buf.length) throw new Error("Cannot save disk page");
        if (!exists) {
          if (buf.length !== PAGE) pages.truncate((slot + 1) * PAGE);
          view.setBigUint64(0, BigInt(base), true);
          if (index.write(batch.subarray(0, 8), { at: slot * 8 }) !== 8)
            throw new Error("Cannot save disk index");
          records.set(base, slot);
        }
      },
    });
    return {
      ...overlay,
      async copyTo(directory) {
        for (const [name, source] of [
          ["pages", pages],
          ["index", index],
        ]) {
          const handle = await directory.getFileHandle(name, { create: true });
          const dest = await handle.createWritable();
          try {
            const buffer = new Uint8Array(1024 * 1024);
            for (let at = 0, end = source.getSize(); at < end;) {
              options?.signal?.throwIfAborted();
              const part = buffer.subarray(0, Math.min(end - at, buffer.length));
              if (source.read(part, { at }) !== part.length) throw Error("Incomplete disk copy");
              await dest.write(part);
              at += part.length;
            }
            await dest.close();
          } catch (e) {
            await dest.abort().catch(() => {});
            throw e;
          }
        }
      },
      flushDisk() {
        if (!dirty) return;
        pages.flush();
        index.flush();
        dirty = false;
      },
      close() {
        try {
          pages.close();
        } finally {
          index.close();
        }
      },
    };
  } catch (e) {
    try {
      pages.close();
    } finally {
      index.close();
    }
    throw e;
  }
}
