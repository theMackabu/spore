#!/usr/bin/env python3
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import termios
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = Path(os.environ.get("BUILD_DIR", ROOT / "target/release")).resolve()
CLI = BUILD / "spore"


def invoke(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def contract(d, disk):
    cc = "aarch64-unknown-linux-musl-gcc"
    ring = d / "uring"
    invoke(cc, "-O2", "-static", "-pthread", HERE / "uring.c", "-o", ring)
    invoke(CLI, "run", ring, timeout=15)
    lib = d / "libfixture.so"
    src = d / "library.c"
    src.write_text("int fixture(void) { return 42; }\n")
    invoke(cc, "-shared", "-fPIC", src, "-o", lib)
    for dynamic in (False, True):
        exe = d / "probe"
        flags = ["-DWITH_LIBRARY", "-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1", "-L" + str(d), "-lfixture"] if dynamic else ["-static"]
        invoke(cc, "-O2", "-pthread", HERE / "probe.c", *flags, "-o", exe)
        img = d / "probe.img"
        invoke(CLI, "build", exe, "-o", img, "--lib-dir", d, "--argv0", "custom name",
               "--cwd", "/work", "--env", "CHECK=a=b", "--", "two words", "")
        result = d / "probe.json"
        p = subprocess.run([CLI, "boot", img, "--result", result], stdout=subprocess.PIPE, timeout=45)
        assert p.returncode == 42 and b"app contract passed" in p.stdout and json.loads(result.read_text())["kind"] == "exit", (p.returncode, p.stdout[-400:])
    lib.unlink()
    missing = subprocess.run([CLI, "build", exe, "-o", d/"missing.img"], capture_output=True)
    assert missing.returncode and b"libfixture.so" in missing.stderr and not (d/"missing.img").exists()
    invoke(cc, "-O2", "-static", "-pthread", HERE / "probe.c", "-o", exe)
    invoke(CLI, "run", exe, "--data", disk, "--", "orphan")
    invoke("e2fsck", "-fn", disk)
    result = d/"signal.json"
    p = subprocess.run([CLI, "run", exe, "--result", result, "--", "signal"], stdout=subprocess.DEVNULL, timeout=45)
    assert p.returncode == 143 and json.loads(result.read_text())["kind"] == "signal"
    result = d/"boot.json"
    p = subprocess.run([CLI, "run", exe, "--cwd", "/data", "--result", result], stdout=subprocess.DEVNULL, timeout=45)
    assert p.returncode == 125 and json.loads(result.read_text())["kind"] == "boot-error"


def forwarded_port():
    with socket.socket() as available:
        available.bind(("127.0.0.1", 0))
        host = available.getsockname()[1]
    guest = 9000
    for flags, expected in [([], guest), (["--env", "PORT=7000"], 7000)]:
        app = invoke(CLI, "run", "ant", "-p", f"{host}:{guest}", *flags,
                     "--", "-e", "console.log(process.env.PORT)", capture_output=True, timeout=10)
        assert app.stdout.strip() == str(expected).encode(), app.stdout


def repl_prompt():
    master, slave = os.openpty()
    try:
        with subprocess.Popen([CLI, "run", "ant"], stdin=slave, stdout=subprocess.PIPE,
                              start_new_session=True,
                              preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0)) as vm:
            def expect(marker):
                output = bytearray()
                deadline = time.monotonic() + 30
                while marker not in output:
                    if not select.select([vm.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                        raise AssertionError(f"Ant timed out waiting for {marker!r}: {output[-200:]!r}")
                    chunk = os.read(vm.stdout.fileno(), 4096)
                    if not chunk:
                        raise AssertionError(f"Ant exited waiting for {marker!r}: {output[-200:]!r}")
                    output.extend(chunk)
            try:
                expect("❯".encode())
                time.sleep(.3)
                os.write(master, b"\x03")
                expect(b"press Ctrl+C again")
                time.sleep(.3)
                os.write(master, b"\x03")
                assert vm.wait(timeout=5) == 0
            finally:
                if vm.poll() is None:
                    vm.terminate()
    finally:
        os.close(master)
        os.close(slave)


def service(d, image, ram, revision, operation, disk):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    def get(path):
        with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=.25) as response:
            return response.read().decode()
    report = d / f"{ram}-{operation[1:]}.json"
    log = d / f"{ram}-{operation[1:]}.log"
    with log.open("wb") as output:
        start = time.monotonic()
        vm = subprocess.Popen([CLI, "boot", image, "--memory", f"{ram}M", "--smp", "1",
                               "--boot-timeout", "14", "--result", report, "--forward", f"{port}:8080",
                               "--data", disk], stdout=output, stderr=output)
        try:
            while vm.poll() is None and time.monotonic()-start < 25:
                try:
                    body = get("/")
                except (OSError, urllib.error.URLError):
                    time.sleep(.02)
                    continue
                assert body == revision, body
                break
            else:
                lines = log.read_text(errors="replace").splitlines()
                reason = next((line for line in lines if "ASSERT" in line), lines[-1] if lines else "no guest output")
                return {"ram_mib": ram, "passed": False, "reason": reason}
            first = time.monotonic()-start
            for _ in range(5):
                assert get("/") == revision
            time.sleep(.5)
            system = get("/system-memory")
            free = int(re.search(r"MemFree:\s+(\d+) kB", system)[1])
            rss = int(re.search(r"VmRSS:\s+(\d+) kB", get("/memory"))[1])
            assert get(operation) == ("saved" if operation == "/save" else "written by one")
            assert get("/quit") == "bye"
            assert vm.wait(timeout=8) == 0
            res = json.loads(report.read_text())
            assert res["kind"] == "exit"
            return dict(ram_mib=ram, passed=True, entry_ms=round(res["entry_us"]/1000,1),
                        http_ms=round(first*1000,1), rss_kib=rss, guest_used_kib=ram*1024-free, image_bytes=image.stat().st_size)
        finally:
            if vm.poll() is None:
                vm.terminate()
                vm.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sweep", action="store_true")
    parser.add_argument("--min-ram", type=int, default=128)
    parser.add_argument("--skip-contract", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="spore-appliance-test-") as tmp:
        d = Path(tmp)
        disk = d/"data.ext2"
        invoke("mke2fs","-q","-F","-t","ext2","-b","4096",disk,"4096")
        if not args.skip_contract:
            contract(d, disk)
            repl_prompt()
            forwarded_port()
        images = []
        for rev in ("one","two"):
            img=d/(rev+".img")
            invoke(CLI,"build", "ant","-o",img,"--file",f"{HERE}/server.js=/app/server.js",
                   "--env",f"REVISION={rev}", "--ip","10.73.1.20","--gateway","10.73.1.2",
                   "--dns","10.73.1.3","--","/app/server.js")
            images.append(img)
        results=[]
        for ram in ([32,64,128] if args.sweep else [args.min_ram]):
            row=service(d,images[0],ram,"one","/save",disk)
            results.append(row)
            print(json.dumps(row),flush=True)
        assert any(row["passed"] and row["ram_mib"]==args.min_ram for row in results),results
        replacement=service(d,images[1],args.min_ram,"two","/load",disk)
        assert replacement["passed"],replacement
        invoke("e2fsck","-fn",disk)
        print("Replacement image read the first image's data",flush=True)


if __name__ == "__main__":
    main()
