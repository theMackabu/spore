import { run } from "./run.mjs";

onmessage = async ({ data: { elf, disk, args } }) => {
  try {
    const response = await fetch("./spore_wasm.wasm");
    if (!response.ok) throw new Error("Build the module with spore build --wasm first.");
    const reader = new FileReaderSync();
    const status = await run(await response.arrayBuffer(), new Uint8Array(await elf.arrayBuffer()), {
      argv0: elf.name,
      args,
      readDisk:
        disk &&
        ((offset, dest) => {
          if (offset < 0n || offset + BigInt(dest.length) > BigInt(disk.size)) return 0;
          const start = Number(offset);
          const data = reader.readAsArrayBuffer(disk.slice(start, start + dest.length));
          dest.set(new Uint8Array(data));
          return data.byteLength;
        }),
      write(fd, bytes) {
        postMessage({ fd, bytes }, [bytes.buffer]);
      },
    });
    postMessage({ status: `Exited with ${status}` });
  } catch (error) {
    postMessage({ status: error.message });
  }
};
