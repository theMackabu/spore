#!/usr/bin/env python3
import shutil
import subprocess
import sys
from pathlib import Path


def read_manifest(path: Path):
    entries = []
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2:
            raise SystemExit(f"bad manifest line: {raw}")
        entries.append((parts[0], parts[1]))
    return entries


def module_name(target: str) -> str:
    return target.strip("/").replace("/", "__") or "init"


def prebuilt_userland(prebuilt_root: Path, source_root: Path, source: str):
    src = source_root / source
    name = src.name if src.is_dir() else src.stem
    candidate = prebuilt_root / source / name
    return candidate if candidate.exists() else None


def compile_userland(source_root: Path, source: str, output: Path, prebuilt_root: Path | None):
    if prebuilt_root is not None:
        prebuilt = prebuilt_userland(prebuilt_root, source_root, source)
        if prebuilt is not None:
            shutil.copy2(prebuilt, output)
            return

    src = source_root / source
    cmd = [
        "zig",
        "cc",
        "-target",
        "aarch64-linux-musl",
        "-static",
        "-std=c23",
        "-D_GNU_SOURCE",
    ]
    if src.is_dir():
        cmd += [
            f"-I{source_root / 'userland/lib/spore'}",
            str(src / "main.c"),
            str(source_root / "userland/lib/spore/util.c"),
        ]
    else:
        cmd += [str(src)]
    cmd += ["-o", str(output)]
    subprocess.run(cmd, check=True)


def copy_into_efi(efi_img: Path, src: Path, dst: str):
    subprocess.run(["mcopy", "-i", str(efi_img), str(src), f"::{dst}"], check=True)


def main() -> int:
    if len(sys.argv) not in (7, 8, 9):
        print(
            "usage: build_image.py SOURCE_ROOT ISO_ROOT KERNEL_ELF BOOT_EFI "
            "OUTPUT_ISO OUTPUT_COPY [MANIFEST] [PREBUILT_ROOT]",
            file=sys.stderr,
        )
        return 2

    source_root = Path(sys.argv[1])
    iso_root = Path(sys.argv[2])
    kernel_elf = Path(sys.argv[3])
    boot_efi = Path(sys.argv[4])
    output_iso = Path(sys.argv[5])
    output_copy = Path(sys.argv[6])
    manifest = Path(sys.argv[7]) if len(sys.argv) >= 8 else source_root / "userland/image.manifest"
    prebuilt_root = Path(sys.argv[8]) if len(sys.argv) == 9 else None

    if iso_root.exists():
        shutil.rmtree(iso_root)
    (iso_root / "boot/modules").mkdir(parents=True)
    (iso_root / "EFI/BOOT").mkdir(parents=True)

    shutil.copy2(kernel_elf, iso_root / "boot/kernel.elf")
    shutil.copy2(boot_efi, iso_root / "EFI/BOOT/BOOTAA64.EFI")

    module_lines = ["# esp-path baked-path"]
    for source, target in read_manifest(manifest):
        name = module_name(target)
        out = iso_root / "boot/modules" / name
        compile_userland(source_root, source, out, prebuilt_root)
        module_lines.append(f"boot/modules/{name} {target}")
    (iso_root / "boot/modules.txt").write_text("\n".join(module_lines) + "\n")

    efi_img = iso_root / "efi.img"
    subprocess.run(["dd", "if=/dev/zero", f"of={efi_img}", "bs=1M", "count=64"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["mkfs.fat", "-F", "32", str(efi_img)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["mmd", "-i", str(efi_img), "::/EFI", "::/EFI/BOOT", "::/boot", "::/boot/modules"], check=True)
    copy_into_efi(efi_img, boot_efi, "/EFI/BOOT/BOOTAA64.EFI")
    copy_into_efi(efi_img, kernel_elf, "/boot/kernel.elf")
    copy_into_efi(efi_img, iso_root / "boot/modules.txt", "/boot/modules.txt")
    for module in sorted((iso_root / "boot/modules").iterdir()):
        copy_into_efi(efi_img, module, f"/boot/modules/{module.name}")

    subprocess.run(
        [
            "xorriso",
            "-as",
            "mkisofs",
            "-R",
            "-r",
            "-J",
            "-hfsplus",
            "-apm-block-size",
            "2048",
            "--efi-boot",
            "efi.img",
            "-no-emul-boot",
            "-efi-boot-part",
            "--efi-boot-image",
            "--protective-msdos-label",
            str(iso_root),
            "-o",
            str(output_iso),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    shutil.copy2(output_iso, output_copy)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
