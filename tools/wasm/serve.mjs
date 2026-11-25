import { createHash } from "node:crypto";
import { createServer } from "node:http";
import { createReadStream } from "node:fs";
import { open } from "node:fs/promises";
import { bootPages } from "./boot-pages.mjs";
import { join, extname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { userNetwork } from "./net.mjs";

const [port, qemu, wsModule] = process.argv.slice(2);
const root = fileURLToPath(new URL(".", import.meta.url));
const assets = new Set([
  "index.html",
  "run.mjs",
  "worker.mjs",
  "disk.mjs",
  "browser-disk.mjs",
  "input.mjs",
  "persistent.mjs",
  "spore_wasm.wasm",
  "xterm/xterm.mjs",
  "xterm/xterm.css",
  "fit/addon-fit.mjs",
  "xterm/LICENSE",
  "fit/LICENSE",
]);
const mime = {
  ".html": "text/html",
  ".mjs": "text/javascript",
  ".css": "text/css",
  ".wasm": "application/wasm",
};
const disk = await open(join(root, "../root.ext2"), "r");
const diskInfo = await disk.stat();
const boot = await bootPages(disk.fd, diskInfo.size, join(root, "spore_wasm.wasm")).catch(error => {
  console.warn("spore: boot prefetch unavailable:", error.message);
});
const bootPath = boot && "boot.pages?v=" + createHash("sha256").update(boot).digest("hex");
async function serveDisk(req, res) {
  const size = diskInfo.size;
  res.setHeader("Content-Type", "application/octet-stream");
  res.setHeader("Accept-Ranges", "bytes");
  res.setHeader("Last-Modified", diskInfo.mtime.toUTCString());
  res.setHeader("Cache-Control", "no-store");
  res.setHeader("Cross-Origin-Resource-Policy", "same-origin");
  if (req.method === "HEAD") {
    if (bootPath) res.setHeader("Spore-Boot-Pages", "./" + bootPath);
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
      res.write(data.subarray(0, bytesRead), err => err ? reject(err) : resolve()));
  }
  res.end();
}

let address;
const server = createServer((req, res) => {
  if (req.headers.host !== address || !["GET", "HEAD"].includes(req.method)) {
    res.writeHead(403).end();
    return;
  }
  const path = req.url.split("?")[0].slice(1) || "index.html";
  if (path === "root.ext2") {
    serveDisk(req, res).catch(err => res.destroy(err));
    return;
  }
  if (boot && req.url === "/" + bootPath) {
    res.writeHead(200, { "Content-Type": "application/octet-stream", "Content-Encoding": "gzip",
      "Content-Length": boot.length, "Cache-Control": "private, max-age=31536000, immutable", "Cross-Origin-Resource-Policy": "same-origin" });
    res.end(req.method === "HEAD" ? undefined : boot);
    return;
  }
  if (!assets.has(path)) {
    res.writeHead(404).end();
    return;
  }
  res.setHeader("Content-Type", mime[extname(path)] || "text/plain");
  res.setHeader("X-Content-Type-Options", "nosniff");
  if (path === "spore_wasm.wasm") {
    res.setHeader("Spore-Network", qemu ? "on" : "off");
    res.setHeader("Cache-Control", "no-cache");
  }
  if (req.method === "HEAD") {
    res.end();
    return;
  }
  const stream = createReadStream(join(root, path));
  stream.on("error", () => res.writeHead(404).end());
  res.on("close", () => stream.destroy());
  stream.pipe(res);
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
  address = "127.0.0.1:" + server.address().port;
  console.log("http://" + address + "/");
});
function close() {
  for (const ws of connections) ws.terminate();
  server.close();
  server.closeAllConnections();
}
process.once("SIGINT", close);
process.once("SIGTERM", close);
