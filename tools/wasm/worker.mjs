import { run } from "./run.mjs";
import { SerialInput } from "./input.mjs";
import { persistentDisk } from "./persistent.mjs";
import { snapshotDisk } from "./disk.mjs";

async function connectNetwork() {
  const url = new URL("/net", location.href);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  const frames = [];
  await new Promise((resolve, reject) => {
    ws.onmessage = e => {
      if (e.data === "ready") resolve();
      else if (e.data instanceof ArrayBuffer && e.data.byteLength <= 1518 && frames.length < 256) {
        frames.push(new Uint8Array(e.data));
      }
    };
    ws.onclose = () => reject(new Error("network relay disconnected"));
    ws.onerror = () => reject(new Error("network relay unavailable"));
    controller.signal.addEventListener("abort", () => ws.close(), {once:true});
  });
  ws.onclose = () => controller.abort(new Error("network relay disconnected"));
  return {
    receive: () => frames.shift(),
    send(frame) {
      if (ws.readyState !== WebSocket.OPEN || ws.bufferedAmount > 1 << 20) return false;
      ws.send(frame);
      return true;
    },
    close() { ws.onclose = null; ws.close(); },
  };
}

const serial = new SerialInput();
const controller = new AbortController();
let dimensions;
onmessage = async ({ data: { elf, disk, args, input, online, persist, stop, initialSize, size, memoryMiB, cpuCount } }) => {
  if (stop) { controller.abort(new Error("Stopped")); return; }
  if (size) { dimensions = size; return; }
  if (input !== undefined) {
    serial.push(new TextEncoder().encode(input));
    return;
  }
  dimensions = initialSize;
  let media, network, result;
  try {
    const response = await fetch("./spore_wasm.wasm");
    if (!response.ok) throw new Error("Build the module with spore build --wasm first.");
    const reader = new FileReaderSync();
    if (disk) {
      const readDisk = (offset, dest) => {
        if (offset < 0n || offset + BigInt(dest.length) > BigInt(disk.size)) return 0;
        const start = Number(offset);
        const data = reader.readAsArrayBuffer(disk.slice(start, start + dest.length));
        dest.set(new Uint8Array(data));
        return data.byteLength;
      };
      media = persist ? await persistentDisk(disk, readDisk) : snapshotDisk(disk.size, readDisk);
    }
    if (online) network = await connectNetwork();
    const status = await run(await response.arrayBuffer(), elf ? new Uint8Array(await elf.arrayBuffer()) : null, {
      ...media,
      network,
      signal: controller.signal,
      memoryMiB, cpuCount,
      argv0: elf?.name,
      args,
      readByte: serial.readByte,
      winsize: () => dimensions,
      instructionLimit: elf ? undefined : Number.MAX_SAFE_INTEGER,
      write(fd, bytes) {
        postMessage({ fd, bytes }, [bytes.buffer]);
      },
    });
    result = `Exited with ${status}`;
  } catch (error) {
    result = error.message || error.name || String(error);
  } finally { network?.close(); media?.close?.(); }
  postMessage({ status: result });
};
