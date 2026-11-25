import { diskKey, openOverlay } from "./persistent.mjs";
import { snapshotDisk } from "./disk.mjs";

const validId = (id) => typeof id === "string" && /^[a-f0-9]{64}$/.test(id);
async function digest(bytes) {
  return Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (b) =>
    b.toString(16).padStart(2, "0"),
  ).join("");
}

async function baseDisk(site, id, warmURL) {
  if (!validId(id)) throw Error("Invalid release disk ID");
  const response = await fetch(new URL("disks/" + id + ".json", site));
  if (!response.ok) throw Error("Missing published disk " + id);
  const bytes = await response.arrayBuffer();
  if ((await digest(bytes)) !== id) throw Error("Published disk descriptor failed verification");
  const { size, chunkSize, chunks } = JSON.parse(new TextDecoder().decode(bytes));
  if (
    !Number.isSafeInteger(size) ||
    size <= 0 ||
    chunkSize !== 256 * 1024 ||
    !chunks ||
    !Object.values(chunks).every(validId)
  )
    throw Error("Invalid disk descriptor");
  const cache = new Map(),
    warm = new Map();
  if (warmURL) {
    const pack = await fetch(warmURL);
    if (pack.ok) {
      const packed = await pack.blob();
      const header = new Uint8Array(await packed.slice(0, 2).arrayBuffer());
      const stream =
        header[0] === 31 && header[1] === 139
          ? packed.stream().pipeThrough(new DecompressionStream("gzip"))
          : packed.stream();
      const data = new Uint8Array(await new Response(stream).arrayBuffer());
      if (data.length % 4100) throw Error("Invalid boot page bundle");
      const view = new DataView(data.buffer);
      for (let i = 0; i < data.length; i += 4100)
        warm.set(view.getUint32(i, true), data.subarray(i + 4, i + 4100));
    }
  }
  function read(off, dest) {
    if (off < 0n || off + BigInt(dest.length) > BigInt(size)) return 0;
    let pos = Number(off);
    for (let done = 0; done < dest.length;) {
      const page = warm.get(Math.floor(pos / 4096));
      const span = page ? 4096 : chunkSize;
      const offset = pos % span;
      const n = Math.min(dest.length - done, span - offset);
      const hash = chunks[Math.floor(pos / chunkSize)];
      let chunk = page || cache.get(hash);
      if (!chunk && hash) {
        const req = new XMLHttpRequest();
        req.open("GET", new URL("objects/" + hash, site), false);
        req.responseType = "arraybuffer";
        req.send();
        if (
          req.status !== 200 ||
          req.response?.byteLength !== Math.min(chunkSize, size - (pos - (pos % chunkSize)))
        )
          throw Error("Failed to read release disk object " + hash);
        chunk = new Uint8Array(req.response);
        if (cache.size === 128) cache.delete(cache.keys().next().value);
        cache.set(hash, chunk);
      }
      if (chunk) dest.set(chunk.subarray(offset, offset + n), done);
      else dest.fill(0, done, done + n);
      pos += n;
      done += n;
    }
    return dest.length;
  }
  return { size, read };
}

export async function mergeVolumes(module, old, incoming, local) {
  let memory, failure;
  const disks = [old.read, incoming.read, local.readDisk];
  const { instance } = await WebAssembly.instantiate(module, {
    storage: {
      now: () => Math.floor(Date.now() / 1000),
      read: (id, off, ptr, len) => disks[id](off, new Uint8Array(memory.buffer, ptr >>> 0, len)) === len,
      write: (id, off, ptr, len) =>
        id === 2 && local.writeDisk(off, new Uint8Array(memory.buffer, ptr >>> 0, len)) === len,
      message: (ptr, len) => {
        failure = new TextDecoder().decode(new Uint8Array(memory.buffer, ptr >>> 0, len));
        console.info("[spore:update]", failure);
      },
    },
  });
  memory = instance.exports.memory;
  if (!instance.exports.upgrade()) throw Error(failure || "Filesystem upgrade failed");
}

async function select(channel, state) {
  const file = await channel.getFileHandle("current.json", { create: true });
  const out = await file.createWritable();
  try {
    await out.write(JSON.stringify(state));
    await out.close();
  } catch (error) {
    await out.abort().catch(() => {});
    throw error;
  }
}

async function install(site, release, latest, key, options) {
  const root = await navigator.storage.getDirectory();
  const channel = await root.getDirectoryHandle(key, { create: true });
  let state;
  try {
    const file = await channel.getFileHandle("current.json");
    const text = await (await file.getFile()).text();
    if (text) state = JSON.parse(text);
  } catch (error) {
    if (error.name !== "NotFoundError") throw error;
  }
  if (!state) {
    const folder = await diskKey(
      { name: new URL("root.ext2", site).href, size: latest.size, lastModified: release.modified },
      latest.read,
    );
    const media = await openOverlay(folder, latest.size, latest.read, options);
    try {
      await select(channel, { folder, backing: release.disk, applied: release.disk });
      return media;
    } catch (error) {
      media.close();
      throw error;
    }
  }
  const slots = [key + "-a", key + "-b"];
  if (
    !validId(state.applied) ||
    !validId(state.backing) ||
    !(slots.includes(state.folder) || /^spore-[a-f0-9]{64}$/.test(state.folder))
  )
    throw Error("Invalid saved installation");
  if (state.applied === release.disk && !options.resetDisk) {
    const base = state.backing === release.disk ? latest : await baseDisk(site, state.backing);
    return openOverlay(state.folder, base.size, base.read, options);
  }
  const folder = slots.find((slot) => slot !== state.folder);
  // An interrupted copy can only occupy the inactive slot.
  try {
    await root.removeEntry(folder, { recursive: true });
  } catch (error) {
    if (error.name !== "NotFoundError") throw error;
  }
  let local,
    previous,
    committed = false;
  try {
    const base =
      options.resetDisk || state.backing === release.disk ? latest : await baseDisk(site, state.backing);
    if (!options.resetDisk) {
      if (base.size !== latest.size)
        throw Error("Cannot merge disks with different capacities. Your saved installation was retained.");
      options.notice("Updating installed files…");
      previous = await openOverlay(state.folder, base.size, base.read, options);
      const target = await root.getDirectoryHandle(folder, { create: true });
      await previous.copyTo(target);
    }
    local = await openOverlay(folder, base.size, base.read, options);
    if (!options.resetDisk) {
      const old = state.applied === state.backing ? base : await baseDisk(site, state.applied);
      const response = await fetch(new URL("./upgrade.wasm", import.meta.url));
      if (!response.ok) throw Error("Cannot load the filesystem updater");
      await mergeVolumes(await response.arrayBuffer(), old, latest, local);
      local.flushDisk();
    }
    options.signal.throwIfAborted();
    await select(channel, {
      folder,
      backing: options.resetDisk ? release.disk : state.backing,
      applied: release.disk,
    });
    committed = true;
    previous?.close();
    previous = null;
    await root
      .removeEntry(state.folder, { recursive: true })
      .catch((error) => console.warn("[spore:update] Previous disk retained", error));
    return local;
  } finally {
    previous?.close();
    if (!committed) {
      local?.close();
      await root.removeEntry(folder, { recursive: true }).catch(() => {});
    }
  }
}

export async function releaseDisk(site, release, persist, options) {
  const latest = await baseDisk(site, release.disk, new URL("./boot.pages.gz", import.meta.url));
  if (!persist) return snapshotDisk(latest.size, latest.read);
  const key = "spore-site-" + (await digest(new TextEncoder().encode(site)));
  const opened = Promise.withResolvers();
  const closed = Promise.withResolvers();
  const notice = setTimeout(() => options.notice("Opening saved installation…"), 1000);
  navigator.locks
    .request(key, { signal: options.signal }, async () => {
      const current = await fetch(new URL("release.json", site), {
        cache: "no-store",
        signal: options.signal,
      });
      if (!current.ok) throw Error("Cannot check the active release");
      if ((await current.json()).id !== release.id)
        throw new DOMException("A newer release is available", "ReleaseChanged");
      const media = await install(site, release, latest, key, options);
      opened.resolve({
        ...media,
        close() {
          try {
            media.close();
          } finally {
            closed.resolve();
          }
        },
      });
      await closed.promise;
    })
    .catch(opened.reject);
  try {
    return await opened.promise;
  } finally {
    clearTimeout(notice);
  }
}
