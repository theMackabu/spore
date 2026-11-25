import { snapshotDisk } from "./disk.mjs";

const PAGE = 4096;
async function openFile(dir, name) {
  const file = await dir.getFileHandle(name, { create: true });
  return file.createSyncAccessHandle();
}

export async function persistentDisk(file, read) {
  const header = new Uint8Array(await file.slice(0, 65536).arrayBuffer());
  const identity = new TextEncoder().encode(JSON.stringify([file.name, file.size, file.lastModified]));
  const bytes = new Uint8Array(identity.length + header.length);
  bytes.set(identity);
  bytes.set(header, identity.length);
  const hash = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  const key = Array.from(hash, (b) => b.toString(16).padStart(2, "0")).join("");
  const root = await navigator.storage.getDirectory();
  const dir = await root.getDirectoryHandle("spore-" + key, { create: true });
  const index = await openFile(dir, "index");
  let pages;
  try {
    pages = await openFile(dir, "pages");
    const records = new Map();
    const entries = Math.floor(index.getSize() / 8);
    if (entries > Math.ceil(file.size / PAGE) || pages.getSize() < entries * PAGE)
      throw new Error("Incomplete persistent disk data");
    const batch = new Uint8Array(8192);
    const view = new DataView(batch.buffer);
    for (let start = 0; start < entries;) {
      const n = Math.min(batch.length / 8, entries - start);
      if (index.read(batch.subarray(0, n * 8), { at: start * 8 }) !== n * 8)
        throw new Error("Cannot read disk index");
      for (let i = 0; i < n; i++) {
        const base = Number(view.getBigUint64(i * 8, true));
        if (!Number.isSafeInteger(base) || base % PAGE || base >= file.size || records.has(base)) {
          throw new Error("Invalid persistent disk index");
        }
        records.set(base, start + i);
      }
      start += n;
    }
    const overlay = snapshotDisk(file.size, read, {
      get(base) {
        const slot = records.get(base);
        if (slot === undefined) return undefined;
        const buf = new Uint8Array(Math.min(PAGE, file.size - base));
        if (pages.read(buf, { at: slot * PAGE }) !== buf.length)
          throw new Error("Cannot read saved disk page");
        return buf;
      },
      set(base, buf) {
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
      flushDisk() {
        pages.flush();
        index.flush();
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
    pages?.close();
    index.close();
    throw e;
  }
}
