import { run } from "./run.mjs";
import { SerialInput } from "./input.mjs";
import { openDisk } from "./browser-disk.mjs";

async function connectNetwork() {
  const url = new URL("/net", location.href);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  const frames = [];
  await new Promise((resolve, reject) => {
    ws.onmessage = (e) => {
      if (e.data === "ready") resolve();
      else if (e.data instanceof ArrayBuffer && e.data.byteLength <= 1518 && frames.length < 256) {
        frames.push(new Uint8Array(e.data));
      }
    };
    ws.onclose = () => reject(new Error("network relay disconnected"));
    ws.onerror = () => reject(new Error("network relay unavailable"));
    controller.signal.addEventListener("abort", () => ws.close(), { once: true });
  });
  ws.onclose = () => controller.abort(new Error("network relay disconnected"));
  return {
    receive: () => frames.shift(),
    send(frame) {
      if (ws.readyState !== WebSocket.OPEN || ws.bufferedAmount > 1 << 20) return false;
      ws.send(frame);
      return true;
    },
    close() {
      ws.onclose = null;
      ws.close();
    },
  };
}

const serial = new SerialInput();
const controller = new AbortController();
let dimensions;
onmessage = async ({
  data: { app, disk, args, input, online, persist, stop, initialSize, size, memoryMiB, cpuCount },
}) => {
  if (stop) {
    controller.abort(new Error("Stopped"));
    return;
  }
  if (size) {
    dimensions = size;
    return;
  }
  if (input !== undefined) {
    serial.push(new TextEncoder().encode(input));
    return;
  }
  dimensions = initialSize;
  let media, network, result;
  try {
    const response = await fetch("./spore_wasm.wasm");
    if (!response.ok) throw new Error("Build the module with spore build --wasm first.");
    if (disk) media = await openDisk(disk, persist);
    let elf = null;
    if (app instanceof File) {
      elf = new Uint8Array(await app.arrayBuffer());
    } else if (app) {
      const response = await fetch(app);
      if (!response.ok) throw new Error(`Cannot open app (${response.status})`);
      elf = new Uint8Array(await response.arrayBuffer());
    }
    if ((response.headers.get("Spore-Network") ?? (online ? "on" : "off")) === "on") {
      network = await connectNetwork();
    }
    const status = await run(
      await response.arrayBuffer(),
      elf,
      {
        ...media,
        checkpointStorage: Boolean(media && persist),
        network,
        signal: controller.signal,
        memoryMiB,
        cpuCount,
        argv0: app instanceof File ? app.name : app ? new URL(app, location.href).pathname.split("/").pop() : undefined,
        args,
        readByte: serial.readByte,
        winsize: () => dimensions,
        instructionLimit: elf ? undefined : Number.MAX_SAFE_INTEGER,
        write(fd, bytes) {
          postMessage({ fd, bytes }, [bytes.buffer]);
        },
      },
    );
    result = `Exited with ${status}`;
  } catch (error) {
    result = error.message || error.name || String(error);
  } finally {
    network?.close();
    media?.close?.();
  }
  postMessage({ status: result });
};
