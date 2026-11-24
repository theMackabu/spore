import argparse
import os
from pathlib import Path
import random
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
BUILD = Path(os.environ.get("BUILD_DIR", HERE.parents[1] / "target/release")).resolve()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=20261124)
    parser.add_argument("--runs", type=int, default=24)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    rng = random.Random(args.seed)
    with tempfile.TemporaryDirectory(prefix="spore-stress-") as directory:
        for linkage in ("static", "dynamic"):
            exe = Path(directory) / linkage
            flags = ["-static"] if linkage == "static" else ["-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1"]
            subprocess.run(["aarch64-unknown-linux-musl-gcc", "-O2", *flags, HERE / "stress.c", "-o", exe], check=True)
        for i in range(args.runs):
            exe = Path(directory) / ("static" if i % 2 else "dynamic")
            seed = rng.randrange(1, 2**32)
            ram = rng.choice((32, 64, 128, 256))
            print(f"run={i + 1} ram={ram}M linkage={exe.name} seed={seed}", flush=True)
            command = [BUILD / "spore", "run", exe, "--memory", f"{ram}M", "--", str(seed)]
            with subprocess.Popen(command) as vm:
                try:
                    code = vm.wait(timeout=45)
                    if code:
                        raise subprocess.CalledProcessError(code, command)
                finally:
                    if vm.poll() is None:
                        vm.terminate()
                        vm.wait(timeout=5)


if __name__ == "__main__":
    main()
