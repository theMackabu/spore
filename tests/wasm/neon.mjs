import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { availableParallelism } from "node:os";
import { mkdtemp, readdir, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { run } from "../../tools/wasm/run.mjs";

const exec = promisify(execFile);
const revision = "fdf984310d6d6b2fa5426fcf66bc227e04609c3f";
const scratch = await mkdtemp(join(tmpdir(), "spore-neon-"));
try {
  const src = process.env.SIMDE_DIR ?? join(scratch, "simde");
  if (!process.env.SIMDE_DIR) {
    await exec("git", ["init", "-q", src]);
    await exec("git", [
      "-C",
      src,
      "fetch",
      "-q",
      "--depth=1",
      "https://github.com/simd-everywhere/simde.git",
      revision,
    ]);
    await exec("git", ["-C", src, "checkout", "-q", "--detach", "FETCH_HEAD"]);
  }
  const head = await exec("git", ["-C", src, "rev-parse", "HEAD"]);
  if (head.stdout.trim() !== revision) throw new Error(`SIMDe suite requires revision ${revision}`);
  const dir = join(src, "test/arm/neon");
  const sources = (await readdir(dir))
    .filter((n) => n.endsWith(".c") && !n.startsWith("skel") && n !== "run-tests.c")
    .sort();
  const mod = await WebAssembly.compile(await readFile(process.argv[2]));
  const errors = [];
  let next = 0;
  await Promise.all(
    Array.from({ length: Math.min(4, availableParallelism()) }, async () => {
      while (next < sources.length) {
        const name = sources[next++];
        const bin = join(scratch, name + ".elf");
        try {
          await exec("aarch64-unknown-linux-musl-gcc", [
            "-DSIMDE_TEST_BARE",
            "-std=gnu11",
            "-static",
            "-O2",
            "-march=armv8-a+crypto+crc",
            "-I",
            src,
            join(dir, name),
            "-lm",
            "-o",
            bin,
          ]);
          let stderr = "";
          const code = await run(mod, await readFile(bin), {
            signal: AbortSignal.timeout(30000),
            write(fd, bytes) {
              if (fd === 2) stderr = (stderr + new TextDecoder().decode(bytes)).slice(-2000);
            },
          });
          if (code) throw new Error(`exit ${code}\n${stderr}`);
        } catch (e) {
          errors.push(name);
          console.error(`${name}: ${e.message}`);
        }
      }
    }),
  );
  console.log(`SIMDe ARM NEON: ${sources.length - errors.length}/${sources.length} executables passed`);
  if (errors.length) process.exitCode = 1;
} finally {
  await rm(scratch, { recursive: true, force: true });
}
