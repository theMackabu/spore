import { assets, publish } from "./publish.mjs";
import { createServer } from "node:http";
import { createReadStream } from "node:fs";
import { open } from "node:fs/promises";
import { join, extname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { userNetwork } from "./net.mjs";

const [port, qemu, wsModule] = process.argv.slice(2);
const root = fileURLToPath(new URL(".", import.meta.url));
const build = join(root, "..");
const site = join(build, "wasm-site");
await publish(build, site, true);
const allowed = new Set([...assets, "boot.pages.gz"]);
const mime = {
  ".json": "application/json",
  ".html": "text/html",
  ".mjs": "text/javascript",
  ".css": "text/css",
  ".wasm": "application/wasm",
};
const disk = await open(join(root, "../root.ext2"), "r");
const diskInfo = await disk.stat();
async function serveDisk(req, res) {
  const size = diskInfo.size;
  res.setHeader("Content-Type", "application/octet-stream");
  res.setHeader("Accept-Ranges", "bytes");
  res.setHeader("Last-Modified", diskInfo.mtime.toUTCString());
  res.setHeader("Cache-Control", "no-store");
  res.setHeader("Cross-Origin-Resource-Policy", "same-origin");
  if (req.method === "HEAD") {
    res.setHeader("Content-Length", size);
    res.end();
    return;
  }
  const range = /^bytes=(\d+)-(\d+)$/.exec(req.headers.range ?? "");
  const start = range ? Number(range[1]) : NaN;
  const end = range ? Math.min(Number(range[2]), size - 1) : NaN;
  if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start > end) {
    res.writeHead(416, { "Content-Range": "bytes */" + size }).end();
    return;
  }
  res.writeHead(206, {
    "Content-Range": `bytes ${start}-${end}/${size}`,
    "Content-Length": end - start + 1,
  });
  for (let offset = start; offset <= end && !res.destroyed;) {
    const data = Buffer.allocUnsafe(Math.min(256 * 1024, end - offset + 1));
    const { bytesRead } = await disk.read(data, 0, data.length, offset);
    if (!bytesRead) throw new Error("Short root disk read");
    offset += bytesRead;
    await new Promise((resolve, reject) =>
      res.write(data.subarray(0, bytesRead), (err) => (err ? reject(err) : resolve())),
    );
  }
  res.end();
}

let address, localhost;
const server = createServer((req, res) => {
  const host = req.headers.host?.toLowerCase();
  if ((host !== address && host !== localhost) || !req.url.startsWith("/") || !["GET", "HEAD"].includes(req.method)) {
    res.writeHead(403).end();
    return;
  }
  if (host === localhost) {
    res.writeHead(307, { Location: "http://" + address + req.url, "Cache-Control": "no-store" }).end();
    return;
  }
  const path = req.url.split("?")[0].slice(1) || "index.html";
  if (path === "root.ext2") {
    serveDisk(req, res).catch((err) => res.destroy(err));
    return;
  }
  const releaseAsset = /^releases\/[a-f0-9]{64}\/(.+)$/.exec(path);
  const immutable =
    (releaseAsset && allowed.has(releaseAsset[1])) ||
    /^objects\/[a-f0-9]{64}$/.test(path) ||
    /^disks\/[a-f0-9]{64}\.json$/.test(path);
  const published = immutable || path === "index.html" || path === "release.json";
  if (!published && !allowed.has(path)) {
    res.writeHead(404).end();
    return;
  }
  res.setHeader("Cache-Control", immutable ? "public, max-age=31536000, immutable" : "no-store");
  res.setHeader("Content-Type", mime[extname(path)] || "application/octet-stream");
  res.setHeader("X-Content-Type-Options", "nosniff");
  if (path === "spore_wasm.wasm" || releaseAsset?.[1] === "spore_wasm.wasm") {
    res.setHeader("Spore-Network", qemu ? "on" : "off");
    res.setHeader("Cache-Control", "no-cache");
  }
  const stream = createReadStream(join(published ? site : root, path));
  stream.on("error", error => {
    if (res.headersSent) res.destroy(error);
    else res.writeHead(error.code === "ENOENT" ? 404 : 500, { "Cache-Control": "no-store" }).end();
  });
  res.on("close", () => stream.destroy());
  if (req.method === "HEAD") stream.once("open", () => res.end());
  else stream.pipe(res);
});

const connections = new Set();
if (qemu) {
  const { WebSocketServer } = await import(pathToFileURL(wsModule));
  const wss = new WebSocketServer({ noServer: true, maxPayload: 1518, perMessageDeflate: false });
  server.on("upgrade", (req, socket, head) => {
    if (req.headers.host !== address || req.headers.origin !== "http://" + address || req.url !== "/net") {
      socket.destroy();
      return;
    }
    wss.handleUpgrade(req, socket, head, (ws) => {
      const abort = new AbortController();
      connections.add(ws);
      let net;
      ws.on("error", () => ws.terminate());
      ws.on("close", () => {
        abort.abort();
        net?.close();
        connections.delete(ws);
      });
      abort.signal.addEventListener("abort", () => ws.terminate(), { once: true });
      const deliver = (frame) => {
        if (ws.readyState === ws.OPEN && ws.bufferedAmount < 1 << 20) ws.send(frame);
      };
      userNetwork(qemu, "user,id=sporenet", abort, deliver).then(
        (link) => {
          if (abort.signal.aborted) {
            link.close();
            return;
          }
          net = link;
          ws.on("message", (data, binary) => {
            if (!binary || data.length < 14) {
              ws.close(1003);
              return;
            }
            net.send(data);
          });
          ws.send("ready");
        },
        () => ws.terminate(),
      );
    });
  });
}
server.on("error", (e) => {
  console.error("spore:", e.message);
  process.exitCode = 1;
});
server.listen(Number(port), "127.0.0.1", () => {
  const suffix = server.address().port === 80 ? "" : ":" + server.address().port;
  address = "127.0.0.1" + suffix;
  localhost = "localhost" + suffix;
  console.log("http://" + address + "/");
});
function close() {
  for (const ws of connections) ws.terminate();
  server.close();
  server.closeAllConnections();
}
process.once("SIGINT", close);
process.once("SIGTERM", close);
