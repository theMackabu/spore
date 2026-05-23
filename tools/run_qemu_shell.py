#!/usr/bin/env python3
import re
import selectors
import subprocess
import sys
import time


CSI_RE = re.compile(rb"\x1b\[[0-9;=?]*[A-Za-z]")
COMMANDS = [
    b"ls /bin\n",
    b"cat /etc/motd\n",
    b"echo hi > /tmp/f\n",
    b"cat /tmp/f\n",
    b"mkdir /tmp/d && cd /tmp/d && touch x && ls\n",
    b"/bin/hello\n",
    b"/bin/spore_demo\n",
    b"confine compute-only /demos/spinner\n",
    b"confine compute-only /demos/peeker /etc/motd\n",
    b"confine fs:/tmp /demos/peeker /etc/motd\n",
    b"confine fs:/tmp /demos/writer /tmp/d/out\n",
    b"confine mem:1 /demos/memhog\n",
    b"runc bad-manifest /demos/escalate\n",
    b"exit\n",
]


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: run_qemu_shell.py QEMU [ARGS...]", file=sys.stderr)
        return 2

    proc = subprocess.Popen(
        sys.argv[1:],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    out = bytearray()
    sent = 0
    last_prompt_at = -1
    deadline = time.monotonic() + 75
    while proc.poll() is None:
        if time.monotonic() > deadline:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
            break
        for key, _ in selector.select(timeout=0.05):
            chunk = key.fileobj.read1(512)
            if not chunk:
                continue
            out += chunk
            clean = CSI_RE.sub(b"", bytes(out))
            prompt_at = clean.rfind(b" $ ")
            if sent < len(COMMANDS) and prompt_at >= 0 and prompt_at != last_prompt_at:
                proc.stdin.write(COMMANDS[sent])
                proc.stdin.flush()
                sent += 1
                last_prompt_at = prompt_at

    if proc.stdout is not None:
        out += proc.stdout.read()
    clean = CSI_RE.sub(b"", bytes(out))
    sys.stdout.buffer.write(clean)
    if clean and not clean.endswith(b"\n"):
        print()
    return proc.returncode if proc.returncode is not None else 124


if __name__ == "__main__":
    raise SystemExit(main())
