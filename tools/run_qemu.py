#!/usr/bin/env python3
import argparse
import os
import re
import selectors
import subprocess
import sys
import time


CSI_RE = re.compile(rb"\x1b\[[0-9;=?]*[A-Za-z]")
INTERESTING_RE = re.compile(rb"(\[spore\][^\r\n]*|\[cell [0-9]+\][^\r\n]*|\[kernel\] lower sync fault[^\r\n]*)")
STDIN_TRIGGER = b"[spore] stdin demo: child blocking on read(0)"
SHELL_COMMANDS = [
    b"ls /bin\n",
    b"cat /etc/motd\n",
    b"echo hi > /tmp/f\n",
    b"cat /tmp/f\n",
    b"mkdir /tmp/d && cd /tmp/d && touch x && ls\n",
    b"/bin/hello\n",
    b"pthread-demo\n",
    b"udp-echo 10.0.2.2 5555 hi\n",
    b"confine net:none udp-send 10.0.2.2 5555 hi\n",
    b"confine net:udp:10.0.2.2:5555 udp-send 10.0.2.2 5555 hi\n",
    b"confine compute-only /demos/spinner\n",
    b"confine fs:/tmp /demos/peeker /etc/motd\n",
    b"confine fs:/tmp /demos/writer /tmp/d/out\n",
    b"confine mem:1 /demos/memhog\n",
    b"runc bad-manifest /demos/escalate\n",
    b"exit\n",
]


def print_interesting(output: bytes) -> None:
    clean = CSI_RE.sub(b"", output)
    matches = INTERESTING_RE.findall(clean)
    if matches:
        for match in matches:
            print(match.decode("utf-8", errors="replace"))
    else:
        sys.stdout.buffer.write(clean)
        if clean and not clean.endswith(b"\n"):
            print()


def run_filtered(command: list[str]) -> int:
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print_interesting(proc.stdout)
    return proc.returncode


def run_with_input(command: list[str], mode: str) -> int:
    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert proc.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    out = bytearray()
    sent = 0
    stdin_sent = False
    last_prompt_at = -1
    deadline = time.monotonic() + (75 if mode == "shell" else 30)
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
            if mode == "stdin" and not stdin_sent and STDIN_TRIGGER in clean and proc.stdin is not None:
                proc.stdin.write(b"z\n")
                proc.stdin.flush()
                stdin_sent = True
            if mode == "shell":
                prompt_at = clean.rfind(b" $ ")
                if sent < len(SHELL_COMMANDS) and prompt_at >= 0 and prompt_at != last_prompt_at and proc.stdin is not None:
                    proc.stdin.write(SHELL_COMMANDS[sent])
                    proc.stdin.flush()
                    sent += 1
                    last_prompt_at = prompt_at
    if proc.stdout is not None:
        out += proc.stdout.read()
    if mode == "shell":
        clean = CSI_RE.sub(b"", bytes(out))
        sys.stdout.buffer.write(clean)
        if clean and not clean.endswith(b"\n"):
            print()
    else:
        print_interesting(bytes(out))
    return proc.returncode if proc.returncode is not None else 124


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("plain", "filter", "stdin", "shell", "network"), default="filter")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error("missing QEMU command")
    if args.command[0] == "--":
        args.command = args.command[1:]
    if args.mode in ("plain", "network"):
        os.execvp(args.command[0], args.command)
    if args.mode == "filter":
        return run_filtered(args.command)
    return run_with_input(args.command, args.mode)


if __name__ == "__main__":
    raise SystemExit(main())
