import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { once } from "node:events";

export async function userNetwork(qemu, settings, controller, deliver) {
  const process = spawn(
    qemu,
    [
      "-machine",
      "none",
      "-nodefaults",
      "-display",
      "none",
      "-serial",
      "none",
      "-qmp",
      "stdio",
      "-netdev",
      settings,
      "-netdev",
      "socket,id=wire,fd=3",
      "-netdev",
      "hubport,id=lan,hubid=0,netdev=sporenet",
      "-netdev",
      "hubport,id=link,hubid=0,netdev=wire",
    ],
    { stdio: ["pipe", "pipe", "pipe", "pipe"] },
  );
  const wire = process.stdio[3];
  const incoming = [];
  let tail = Buffer.alloc(0),
    errors = "",
    closed = false;
  const fail = (e) => {
    if (!closed) controller.abort(e);
  };
  const close = () => {
    closed = true;
    process.kill();
    wire.destroy();
  };
  process.stderr.on("data", (chunk) => {
    errors = (errors + chunk.toString()).slice(-4096);
  });
  process.on("error", fail);
  process.on("exit", () => fail(new Error(errors.trim() || "network helper stopped")));
  process.stdin.on("error", fail);
  wire.on("error", fail);
  wire.on("data", (chunk) => {
    const data = tail.length ? Buffer.concat([tail, chunk]) : chunk;
    let pos = 0;
    while (pos + 4 <= data.length) {
      const len = data.readUInt32BE(pos);
      if (!len || len > 65536) {
        fail(new Error("invalid Ethernet frame from network helper"));
        return;
      }
      if (pos + 4 + len > data.length) break;
      if (len <= 1518) {
        const frame = data.subarray(pos + 4, pos + 4 + len);
        if (deliver) deliver(frame);
        else if (incoming.length < 256) incoming.push(frame);
      }
      pos += 4 + len;
    }
    tail = data.subarray(pos);
  });
  const monitor = createInterface({ input: process.stdout });
  try {
    const signal = controller.signal;
    const [greeting] = await once(monitor, "line", { signal });
    if (!JSON.parse(greeting).QMP) throw new Error("invalid network helper greeting");
    const reply = once(monitor, "line", { signal });
    process.stdin.write('{"execute":"qmp_capabilities"}\n');
    if (!("return" in JSON.parse((await reply)[0]))) throw new Error("network initialization failed");
  } catch (e) {
    close();
    throw controller.signal.reason ?? e;
  } finally {
    monitor.close();
  }
  return {
    close,
    receive: () => incoming.shift(),
    send(frame) {
      if (wire.destroyed || wire.writableLength > 1 << 20) return false;
      const packet = Buffer.allocUnsafe(4 + frame.length);
      packet.writeUInt32BE(frame.length);
      packet.set(frame, 4);
      wire.write(packet);
      return true;
    },
  };
}
