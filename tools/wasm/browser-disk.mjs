import { persistentDisk } from "./persistent.mjs";
import { snapshotDisk } from "./disk.mjs";

export async function openDisk(source, persist) {
  if (source instanceof File) {
    const reader = new FileReaderSync();
    const read = (offset, dest) => {
      const start = Number(offset);
      const bytes = reader.readAsArrayBuffer(source.slice(start, start + dest.length));
      dest.set(new Uint8Array(bytes));
      return bytes.byteLength;
    };
    return persist ? persistentDisk(source, read) : snapshotDisk(source.size, read);
  }
  const info = await fetch(source, { method: "HEAD" });
  if (!info.ok) throw new Error(`Cannot open root disk (${info.status}). Build it with spore build --userland.`);
  const size = Number(info.headers.get("Content-Length"));
  if (!Number.isSafeInteger(size) || size <= 0 || info.headers.get("Accept-Ranges") !== "bytes")
    throw new Error("The root disk server must support byte ranges and report the disk size.");
  const chunkSize = 256 * 1024;
  const chunks = new Map();
  const warm = new Map();
  const pack = info.headers.get("Spore-Boot-Pages");
  if (pack) {
    const response = await fetch(new URL(pack, info.url));
    if (response.ok) {
      const bytes = new Uint8Array(await response.arrayBuffer());
      const view = new DataView(bytes.buffer);
      if (bytes.length % 4100 === 0) {
        for (let at = 0; at < bytes.length; at += 4100)
          warm.set(view.getUint32(at, true), bytes.subarray(at + 4, at + 4100));
      }
    }
  }
  function read(offset, dest) {
    let pos = Number(offset);
    for (let done = 0; done < dest.length;) {
      const page = warm.get(Math.floor(pos / 4096));
      if (page) {
        const start = pos % 4096;
        const count = Math.min(dest.length - done, 4096 - start);
        dest.set(page.subarray(start, start + count), done);
        done += count;
        pos += count;
        continue;
      }
      const base = pos - pos % chunkSize;
      let chunk = chunks.get(base);
      if (!chunk) {
        const end = Math.min(base + chunkSize, size) - 1;
        const request = new XMLHttpRequest();
        request.open("GET", source, false);
        request.responseType = "arraybuffer";
        request.setRequestHeader("Range", `bytes=${base}-${end}`);
        request.send();
        if (request.status !== 206 || request.response?.byteLength !== end - base + 1)
          throw new Error(`Failed to read root disk at byte ${base}`);
        chunk = new Uint8Array(request.response);
        if (chunks.size === 64) chunks.delete(chunks.keys().next().value);
        chunks.set(base, chunk);
      }
      const count = Math.min(dest.length - done, chunk.length - (pos - base));
      dest.set(chunk.subarray(pos - base, pos - base + count), done);
      done += count;
      pos += count;
    }
    return dest.length;
  }
  return persist
    ? persistentDisk({ name: info.url, size, lastModified: info.headers.get("Last-Modified") }, read)
    : snapshotDisk(size, read);
}
