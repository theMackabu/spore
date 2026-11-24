#!/usr/bin/env python3
import argparse
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = Path(os.environ.get("BUILD_DIR", ROOT / "build")).resolve()
CLI = BUILD / "spore"


def invoke(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def contract(d, disk):
    cc = "aarch64-unknown-linux-musl-gcc"
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


def repl_prompt():
    ant = BUILD / "userland/prebuilt/ant"
    with subprocess.Popen([CLI, "run", ant], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as vm:
        try:
            output = bytearray()
            deadline = time.monotonic() + 30
            while "❯".encode() not in output:
                if not select.select([vm.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                    raise AssertionError("Ant prompt did not appear before input")
                chunk = os.read(vm.stdout.fileno(), 4096)
                if not chunk:
                    raise AssertionError("Ant exited before displaying its prompt")
                output.extend(chunk)
            vm.communicate(b".exit\r", timeout=5)
            assert vm.returncode == 0
        finally:
            if vm.poll() is None:
                vm.terminate()


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
        images = []
        for rev in ("one","two"):
            img=d/(rev+".img")
            invoke(CLI,"build", BUILD/"userland/prebuilt/ant","-o",img,"--file",f"{HERE}/server.js=/app/server.js",
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
