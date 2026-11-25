import { createServer } from "node:http";
import { createReadStream } from "node:fs";
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
let address;
const server = createServer((req, res) => {
  if (req.headers.host !== address || !["GET", "HEAD"].includes(req.method)) {
    res.writeHead(403).end();
    return;
  }
  const path = req.url.split("?")[0].slice(1) || "index.html";
  if (!assets.has(path)) {
    res.writeHead(404).end();
    return;
  }
  res.setHeader("Content-Type", mime[extname(path)] || "text/plain");
  res.setHeader("X-Content-Type-Options", "nosniff");
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
  console.log("http://" + address + (qemu ? "/?net" : "/"));
});
function close() {
  for (const ws of connections) ws.terminate();
  server.close();
  server.closeAllConnections();
}
process.once("SIGINT", close);
process.once("SIGTERM", close);
