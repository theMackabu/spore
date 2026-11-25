import { readFileSync, readSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { run } from "./run.mjs";
import { snapshotDisk } from "./disk.mjs";

export async function bootPages(file, size, kernel) {
  const pageSize = 4096;
  const pages = new Map();
  const stop = new AbortController();
  const timer = setTimeout(() => stop.abort(), 250);
  const read = (offset, dest) => {
    const pos = Number(offset);
    const base = pos - pos % pageSize;
    let page = pages.get(base);
    if (!page) {
      if (pages.size >= 2048) {
        stop.abort();
        return 0;
      }
      page = Buffer.alloc(pageSize);
      const length = Math.min(pageSize, size - base);
      if (readSync(file, page, 0, length, base) !== length) return 0;
      pages.set(base, page);
    }
    dest.set(page.subarray(pos - base, pos - base + dest.length));
    return dest.length;
  };
  try {
    await run(readFileSync(kernel), null, {
      ...snapshotDisk(size, read),
      readByte: () => -1,
      memoryMiB: 1024,
      signal: stop.signal,
      instructionLimit: Number.MAX_SAFE_INTEGER,
    });
  } catch (error) {
    if (!stop.signal.aborted) throw error;
  } finally {
    clearTimeout(timer);
  }
  const result = Buffer.allocUnsafe(pages.size * (4 + pageSize));
  let at = 0;
  for (const [base, page] of pages) {
    result.writeUInt32LE(base / pageSize, at);
    page.copy(result, at + 4);
    at += 4 + pageSize;
  }
  return gzipSync(result, { level: 1 });
}
