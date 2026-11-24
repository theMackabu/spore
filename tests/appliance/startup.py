import argparse
import math
import os
from pathlib import Path
import select
import statistics
import subprocess
import time

BUILD = Path(os.environ.get("BUILD_DIR", Path(__file__).resolve().parents[2] / "build")).resolve()


def launch():
    start = time.monotonic()
    with subprocess.Popen([BUILD / "spore", "run", "ant"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as vm:
        try:
            output = bytearray()
            while "❯".encode() not in output:
                if not select.select([vm.stdout], [], [], max(0, start + 10 - time.monotonic()))[0]:
                    raise TimeoutError("Ant prompt timed out")
                chunk = os.read(vm.stdout.fileno(), 4096)
                if not chunk:
                    raise RuntimeError(f"VM exited before prompt: {output[-300:]!r} {vm.stderr.read()[-500:]!r}")
                output.extend(chunk)
            return (time.monotonic() - start) * 1000
        finally:
            if vm.poll() is None:
                vm.terminate()
            vm.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=30)
    parser.add_argument("--max-p95-ms", type=float)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    samples = [launch() for _ in range(args.runs)]
    p95 = sorted(samples)[math.ceil(.95 * len(samples)) - 1]
    print(f"{len(samples)} fresh VMs, CLI invocation to Ant prompt (ms): first={samples[0]:.1f} "
          f"median={statistics.median(samples):.1f} p95={p95:.1f} max={max(samples):.1f}")
    if args.max_p95_ms is not None and p95 > args.max_p95_ms:
        raise SystemExit("startup budget exceeded")


if __name__ == "__main__":
    main()
