import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { run } from "../../tools/wasm/run.mjs";

const [spore, wasm, mode] = process.argv.slice(2);
const kernel = await WebAssembly.compile(fs.readFileSync(wasm));
const dir = fs.mkdtempSync(path.join(os.tmpdir(), "spore-wasm-"));
const sources = path.dirname(fileURLToPath(import.meta.url));
const exec = (cmd, args, expected = 0, input) => {
  const result = spawnSync(cmd, args, { timeout: 30000, input });
  if (result.error) throw result.error;
  assert.equal(result.status, expected, result.stderr.toString());
  return result.stdout;
};
try {
  const reader = path.join(dir, "read");
  const cpu = path.join(dir, "cpu");
  exec("aarch64-unknown-linux-musl-gcc", [
    "-nostdlib",
    "-static",
    path.join(sources, "cpu.S"),
    "-o",
    cpu,
  ]);
  if (mode === "--bench") {
    const elf = fs.readFileSync(cpu);
    const samples = [[], []];
    let compiled = 0;
    for (let i = 0; i < 6; i++) {
      for (const jit of [!!(i % 2), !(i % 2)]) {
        const stats = {};
        const start = performance.now();
        assert.equal(await run(kernel, elf, { args: ["benchmark"], jit, stats }), 42);
        if (i) samples[Number(jit)].push(performance.now() - start);
        if (jit) compiled = stats.compiledInstructions;
      }
    }
    const median = samples.map(row => row.toSorted((a,b) => a-b)[2].toFixed(1));
    console.log(`16,777,216 ARM64 loop instructions, median of 5 fresh instances per mode: interpreter ${median[0]} ms, JIT ${median[1]} ms (${compiled} instructions compiled).`);
  } else {
    exec("aarch64-unknown-linux-musl-gcc", [
      "-O2",
      "-static",
      path.join(sources, "read.c"),
      "-o",
      reader,
    ]);
    const stdin = Buffer.alloc(192 * 1024);
    for (let i = 0; i < stdin.length; i++) stdin[i] = i & 255;
    assert.deepEqual(exec(spore, ["run", "--wasm", reader], 42, stdin), stdin);
    assert.deepEqual(exec(spore, ["run", "--wasm", reader], 42, Buffer.alloc(0)), Buffer.alloc(0));
    const disk = path.join(dir, "data.ext2");
    const tree = path.join(dir, "root");
    fs.mkdirSync(tree);
    const bytes = Buffer.from("AArch64 through Spore ".repeat(350));
    fs.writeFileSync(path.join(tree, "hello.txt"), bytes);
    exec("mke2fs", ["-q", "-t", "ext2", "-b", "4096", "-O", "none,filetype", "-d", tree, disk, "2048"]);
    for (const backend of [[], ["--wasm"]]) {
      const args = ["run", ...backend, reader, "--data", disk, "--"];
      assert.deepEqual(exec(spore, [...args, "/data/hello.txt"], 42), bytes);
      exec(spore, [...args, "/data/missing"], 2);
      exec(spore, ["run", ...backend, cpu, "--", "ok"], 42);
    }
    const threads = path.join(dir, "threads");
    const probe = path.join(dir, "probe");
    const recovery = path.join(dir, "fault");
    const jit = path.join(dir, "jit");
    const signal = path.join(dir,"signal");
    const copy = path.join(dir,"copy");
    const smp = path.join(dir, "smp");
    exec("aarch64-unknown-linux-musl-gcc", ["-static", "-O2", "-pthread", path.join(sources, "../ports/smp.c"), "-o", smp]);
    exec(spore, ["run", "--wasm", smp, "--smp", "4"]);
    const crypto = path.join(dir, "crypto");
    exec("aarch64-unknown-linux-musl-gcc", ["-static", "-O2", "-march=armv8-a+crypto+crc", path.join(sources, "crypto.c"), "-o", crypto]);
    const native = exec(spore, ["run", crypto], 42);
    assert.equal(native.length, 576);
    assert.deepEqual(exec(spore, ["run", "--wasm", crypto], 42), native);
    const math = path.join(dir, "arithmetic");
    exec("aarch64-unknown-linux-musl-gcc", ["-static", "-nostdlib", path.join(sources, "arithmetic.S"), "-o", math]);
    const expected = exec(spore, ["run", math], 42);
    for (const jit of [false, true]) {
      const chunks = [], stats = {};
      assert.equal(await run(kernel, fs.readFileSync(math), { jit, stats, write: (_, bytes) => chunks.push(bytes) }), 42);
      assert.deepEqual(Buffer.concat(chunks), expected);
      if (jit) assert.ok(stats.compiledInstructions > 0);
    }
    const bsd = path.join(dir, "freebsd");
    exec("clang", ["--target=aarch64-unknown-freebsd", "-nostdlib", "-static", "-fuse-ld=lld", "-Wl,-e,_start",
      "-ffreestanding", "-fno-stack-protector", ...["freebsd-aarch64.S", "freebsd-call.S", "freebsd-services.c"]
        .map(file => path.join(sources, "../ports", file)), "-o", bsd]);
    const asm = ["float", "atomic"].map(name => {
      const out = path.join(dir, name);
      exec("aarch64-unknown-linux-musl-gcc", ["-static", "-nostdlib", path.join(sources, name + ".S"), "-o", out]);
      return out;
    });
    for (const [src, out] of [["threads.c", threads], ["fault.c", recovery], ["jit.c", jit], ["signal.c", signal], ["copy.c", copy], ["../appliance/probe.c", probe]]) {
      exec("aarch64-unknown-linux-musl-gcc", ["-static", "-pthread", "-O2", path.join(sources, src), "-o", out]);
    }
    for (const backend of [[], ["--wasm"]]) {
      assert.equal(exec(spore, ["run", ...backend, bsd, "--", "PASS"], 42).toString().trim(), "FreeBSD OK");
      for (const exe of asm) exec(spore, ["run", ...backend, exe], 42);
      exec(spore, ["run", ...backend, threads], 42);
      exec(spore, ["run", ...backend, recovery], 42);
      exec(spore, ["run", ...backend, jit], 42);
      exec(spore, ["run", ...backend, signal], 42);
      exec(spore, ["run", ...backend, copy], 42);
      exec(spore, ["run", ...backend, cpu, "--", "protect"], 139);
      exec(spore, ["run", ...backend, cpu, "--", "unknown"], 132);
      exec(spore, ["run", ...backend, probe, "--exec", "/app/server", "--argv0", "custom name",
        "--cwd", "/work", "--env", "CHECK=a=b", "--file", path.join(tree, "hello.txt") + "=/work/message",
        "--", "two words", ""], 42);
    }
    const systemDisk = path.join(dir, "system.ext2");
    fs.mkdirSync(path.join(tree, "sbin"));
    exec("aarch64-unknown-linux-musl-gcc", ["-static", "-O2", path.join(sources, "init.c"), "-o", path.join(tree, "sbin/init")]);
    fs.copyFileSync(reader, path.join(tree, "reader"));
    exec("mke2fs", ["-q", "-t", "ext2", "-b", "4096", "-O", "none,filetype", "-d", tree, systemDisk, "2048"]);
    for (let i = 0; i < 2; i++) {
      assert.equal(exec(spore, ["boot", "--wasm", "--root", systemDisk], 42).toString().trim(), "snapshot");
    }
    const persistent = path.join(dir,"persistent.ext2");
    exec(spore, ["boot","--wasm","--root",systemDisk,"--persist",persistent],42);
    exec(spore, ["boot","--wasm","--root",systemDisk,"--persist",persistent],2);
    exec(spore, ["boot","--wasm","--root",systemDisk],42);
    const esp = path.join(dir,"boot.img"), rebootDisk = path.join(dir,"reboot.ext2");
    fs.writeFileSync(esp,Buffer.concat([Buffer.from("ESP!"),Buffer.alloc(508)]));
    fs.writeFileSync(path.join(tree,"reboot-test"),"");
    exec("mke2fs", ["-q","-t","ext2","-b","4096","-O","none,filetype","-d",tree,rebootDisk,"2048"]);
    exec(spore, ["boot","--wasm",esp,"--root",rebootDisk],42);
    const dynamic = path.join(dir, "read-dynamic");
    exec("aarch64-unknown-linux-musl-gcc", ["-O2", path.join(sources, "read.c"), "-o", dynamic]);
    const assets = ["--cwd", "/work", "--file", path.join(tree, "hello.txt") + "=/work/message"];
    for (const backend of [[], ["--wasm"]]) {
      assert.deepEqual(exec(spore, ["run", ...backend, dynamic, ...assets, "--", "message"], 42), bytes);
    }
    const payload = path.join(dir, "read.spore");
    exec(spore, ["build", "--wasm", dynamic, "-o", payload, ...assets, "--", "message"]);
    const report = path.join(dir, "result.json");
    assert.deepEqual(exec(spore, ["boot","--wasm",payload,"--result",report],42), bytes);
    const result = JSON.parse(fs.readFileSync(report));
    assert.equal(result.kind,"exit");
    assert.equal(result.value,42);
    assert.ok(result.entry_us > 0);
    exec(spore, ["run","--wasm",cpu,"--result",report,"--","protect"],139);
    assert.equal(JSON.parse(fs.readFileSync(report)).kind,"signal");
    const chunks = [];
    assert.equal(
      await run(kernel, fs.readFileSync(payload), { write: (_, data) => chunks.push(data) }),
      42,
    );
    assert.deepEqual(Buffer.concat(chunks), bytes);
    const image = fs.readFileSync(cpu);
    for (const [arg, reason] of [
      ["protect", /Write fault/],
      ["x", /Execute fault/],
      ["unknown", /unsupported A64 instruction/],
      ["loop", /instruction limit/],
    ]) {
      await assert.rejects(run(kernel, image, { args: [arg], instructionLimit: 1000 }), reason);
    }
    await assert.rejects(run(kernel, new Uint8Array(64)), /boot error/);
    console.log(
      "Wasm and QEMU passed file, thread, timer and app contract checks. Wasm fault checks passed.",
    );
  }
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}
