import { run } from "./run.mjs";
import { SerialInput } from "./input.mjs";
import { openDisk } from "./browser-disk.mjs";

async function connectNetwork() {
  const url = new URL("/net", location.href);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  const frames = [];
  let wake;
  await new Promise((resolve, reject) => {
    ws.onmessage = (e) => {
      if (e.data === "ready") resolve();
      else if (e.data instanceof ArrayBuffer && e.data.byteLength <= 1518 && frames.length < 256) {
        frames.push(new Uint8Array(e.data));
        wake?.();
      }
    };
    ws.onclose = () => reject(new Error("network relay disconnected"));
    ws.onerror = () => reject(new Error("network relay unavailable"));
    controller.signal.addEventListener("abort", () => ws.close(), { once: true });
  });
  ws.onclose = () => controller.abort(new Error("network relay disconnected"));
  return {
    pending: () => frames.length > 0,
    waitForFrame(delay) {
      if (frames.length) return;
      return new Promise((resolve) => {
        const timer = setTimeout(finish, delay);
        function finish() {
          clearTimeout(timer);
          wake = undefined;
          resolve();
        }
        wake = finish;
      });
    },
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
  data: {
    app,
    disk,
    release,
    site,
    args,
    input,
    online,
    persist,
    resetDisk,
    stop,
    initialSize,
    size,
    memoryMiB,
    cpuCount,
  },
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
  let media,
    network,
    result,
    reload = false;
  const decoder = new TextDecoder();
  let partialLine = "";
  function kernelLog(bytes) {
    const lines = (partialLine + decoder.decode(bytes, { stream: true })).split("\n");
    partialLine = lines.pop();
    for (const line of lines) console.info("[spore:kernel]", line.trimEnd());
  }
  try {
    const response = await fetch("./spore_wasm.wasm");
    if (!response.ok) throw new Error("Build the module with spore build --wasm first.");
    const options = {
      notice: (notice) => postMessage({ notice }),
      resetDisk,
      signal: controller.signal,
      onWaiting(error) {
        console.warn("[spore] Retrying saved disk access", error.name, error.message);
        postMessage({
          notice: "Waiting for browser storage to open the saved disk. Retrying automatically.",
        });
      },
    };
    if (release) {
      const { releaseDisk } = await import("./release-disk.mjs");
      media = await releaseDisk(site, release, persist, options);
    } else if (disk) media = await openDisk(disk, persist, options);
    controller.signal.throwIfAborted();
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
    const info = { arch: "aarch64", cpuCount, memoryMiB, jit: true, network: Boolean(network) };
    console.info("[spore] Booting guest", info);
    const status = await run(await response.arrayBuffer(), elf, {
      ...media,
      checkpointStorage: Boolean(media && persist),
      network,
      signal: controller.signal,
      memoryMiB,
      cpuCount,
      kernelLog,
      onEntry: () => console.info("[spore:cpu] First guest instruction fetch"),
      onException: (pc, reason) => console.warn(`[spore:cpu] Exception at 0x${pc.toString(16)}: ${reason}`),
      onStats: (counters) => postMessage({ stats: { ...info, ...counters } }),
      argv0:
        app instanceof File
          ? app.name
          : app
            ? new URL(app, location.href).pathname.split("/").pop()
            : undefined,
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
    reload = error.name === "ReleaseChanged";
    result = error.message || error.name || String(error);
  } finally {
    partialLine += decoder.decode();
    if (partialLine) console.info("[spore:kernel]", partialLine);
    network?.close();
    media?.close?.();
  }
  postMessage({ status: result, reload });
};
