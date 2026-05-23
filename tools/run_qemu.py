#!/usr/bin/env python3
import re
import subprocess
import sys


CSI_RE = re.compile(rb"\x1b\[[0-9;=?]*[A-Za-z]")
INTERESTING_RE = re.compile(rb"(\[kernel\][^\r\n]*|hello, world)")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: run_qemu.py QEMU [ARGS...]", file=sys.stderr)
        return 2

    proc = subprocess.run(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    clean = CSI_RE.sub(b"", proc.stdout)
    matches = INTERESTING_RE.findall(clean)
    if matches:
        for match in matches:
            print(match.decode("utf-8", errors="replace"))
    else:
        sys.stdout.buffer.write(clean)
        if clean and not clean.endswith(b"\n"):
            print()
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())

