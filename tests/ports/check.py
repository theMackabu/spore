import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile

sources = Path(__file__).resolve().parent
cli = Path(sys.argv[1]).resolve()


def check(directory, target, source, output, status, kind="exit"):
    exe = directory / "probe"
    report = directory / "result.json"
    report.unlink(missing_ok=True)
    extra = [sources / "freebsd-call.S", sources / "freebsd-services.c"] if target.endswith("freebsd") else []
    subprocess.run(["clang", "--target=" + target, "-nostdlib", "-static", "-fuse-ld=lld",
                    "-Wl,-e,_start", *(["-DFAULT"] if kind == "signal" else []),
                    "-ffreestanding", "-fno-stack-protector", sources / source, *extra, "-o", exe], check=True)
    with subprocess.Popen([cli, "run", exe, "--result", report, "--", "PASS"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True) as vm:
        try:
            stdout, stderr = vm.communicate(timeout=45)
        except subprocess.TimeoutExpired:
            os.killpg(vm.pid, signal.SIGKILL)
            raise
        assert vm.returncode == status and stdout.strip() == output, (target, vm.returncode, stdout, stderr)
    result = json.loads(report.read_text())
    assert (result["kind"], result["value"]) == (kind, status - 128 if kind == "signal" else status), result
    print(f"{target} {kind} {result['value']}", flush=True)


with tempfile.TemporaryDirectory(prefix="spore-ports-") as tmp:
    directory = Path(tmp)
    check(directory, "aarch64-unknown-freebsd", "freebsd-aarch64.S", b"FreeBSD OK", 42)
    for arch in ["x86_64", "riscv64"]:
        check(directory, arch + "-unknown-linux-musl", f"linux-{arch}.S", b"PASS", 42)
        check(directory, arch + "-unknown-linux-musl", f"linux-{arch}.S", b"", 132, "signal")
        check(directory, arch + "-unknown-freebsd", f"freebsd-{arch}.S", b"PASS", 42)

    for arch in ("aarch64", "x86_64", "riscv64"):
        exe = directory / ("uring-" + arch)
        subprocess.run(["zig", "cc", "-target", arch + "-linux-musl", "-O2", "-UNDEBUG", "-static",
                        sources.parent / "appliance/uring.c", "-o", exe], check=True)
        subprocess.run([cli, "run", exe], check=True, timeout=45)
        print(f"{arch} mmap, epoll, wait and io_uring passed", flush=True)

        image = directory / (arch + ".img")
        subprocess.run([cli, "build", exe, "-o", image], check=True)
        subprocess.run([cli, "boot", image], check=True, timeout=45)

        smp = directory / ("smp-" + arch)
        subprocess.run(["zig", "cc", "-target", arch + "-linux-musl", "-O2", "-UNDEBUG", "-static",
                        sources / "smp.c", "-o", smp], check=True)
        subprocess.run([cli, "run", smp, "--smp", "4"], check=True, timeout=45)
        print(f"{arch} four CPUs observed and shared address-space changes passed", flush=True)

        if "--userland" in sys.argv[2:]:
            root = directory / arch
            (root / "sbin").mkdir(parents=True)
            subprocess.run(["zig", "cc", "-target", arch + "-linux-musl", "-O2", "-UNDEBUG", "-static",
                            sources / "child.c", "-o", root / "sbin/init"], check=True)
            disk = directory / (arch + ".ext2")
            with disk.open("wb") as f:
                f.truncate(32 << 20)
            subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", "4096", "-O",
                            "none,filetype,sparse_super,large_file", "-d", root, disk], check=True)
            vm = subprocess.run([cli, "boot", "--arch", arch, "--memory", "128M", "--smp", "4",
                                 "--root", disk, "--snapshot"], input=b"", stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=45, check=True)
            assert b"child wait passed" in vm.stdout, (arch, vm.stdout[-1024:])
            print(f"{arch} fork, shared mapping, copy-on-write and waitpid passed", flush=True)
