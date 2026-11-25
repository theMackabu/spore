export function snapshotDisk(size, read) {
  const pages = new Map();
  const pageSize = 4096;
  function transfer(offset, data, writing) {
    if (offset < 0n || offset + BigInt(data.length) > BigInt(size)) return 0;
    for (let done = 0; done < data.length;) {
      const pos = Number(offset) + done;
      const base = pos - pos % pageSize;
      const start = pos - base;
      const len = Math.min(pageSize - start, data.length - done);
      let page = pages.get(base);
      if (writing) {
        if (!page) {
          page = new Uint8Array(Math.min(pageSize, size - base));
          if ((start || len !== page.length) && read(BigInt(base), page) !== page.length) return done;
          pages.set(base, page);
        }
        page.set(data.subarray(done, done + len), start);
      } else if (page) {
        data.set(page.subarray(start, start + len), done);
      } else if (read(BigInt(pos), data.subarray(done, done + len)) !== len) {
        return done;
      }
      done += len;
    }
    return data.length;
  }
  return { readDisk: (off, dest) => transfer(off, dest, false), writeDisk: (off, src) => transfer(off, src, true) };
}
