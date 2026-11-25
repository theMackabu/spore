import { run } from "./run.mjs";
import { SerialInput } from "./input.mjs";
import { snapshotDisk } from "./disk.mjs";

const serial = new SerialInput();
onmessage = async ({ data: { elf, disk, args, input } }) => {
  if (input !== undefined) {
    serial.push(new TextEncoder().encode(input));
    return;
  }
  try {
    const response = await fetch("./spore_wasm.wasm");
    if (!response.ok) throw new Error("Build the module with spore build --wasm first.");
    const reader = new FileReaderSync();
    let media;
    if (disk) {
      const readDisk = (offset, dest) => {
        if (offset < 0n || offset + BigInt(dest.length) > BigInt(disk.size)) return 0;
        const start = Number(offset);
        const data = reader.readAsArrayBuffer(disk.slice(start, start + dest.length));
        dest.set(new Uint8Array(data));
        return data.byteLength;
      };
      media = elf ? { readDisk } : snapshotDisk(disk.size, readDisk);
    }
    const status = await run(await response.arrayBuffer(), elf ? new Uint8Array(await elf.arrayBuffer()) : null, {
      ...media,
      argv0: elf?.name,
      args,
      readByte: elf ? undefined : serial.readByte,
      instructionLimit: elf ? undefined : Number.MAX_SAFE_INTEGER,
      write(fd, bytes) {
        postMessage({ fd, bytes }, [bytes.buffer]);
      },
    });
    postMessage({ status: `Exited with ${status}` });
  } catch (error) {
    postMessage({ status: error.message });
  }
};
