use spore_build::{files, output_dir, run, rust_archive};
use spore_packages::Archive;
use std::{env, path::Path, process::Command};

fn limine(out: &Path) {
    let release = Archive {
        url: "https://github.com/Limine-Bootloader/Limine/releases/download/v12.11.1/limine-binary.zip".into(),
        sha256: "27773b8b974f6ab55d7be5bd15a6914fe70a91f0c56858ca0cbe76c590b60a22".into(),
    };
    let rev = "877c2d4c50e170575f8b8e7fbf7b8fa89b1dc157";
    let protocol = Archive {
        url: format!("https://github.com/Limine-Bootloader/limine-protocol/archive/{rev}.zip"),
        sha256: "f792c62257377208e6f53a31aa3442eb182e9ba451f139749296da9d411f2adb".into(),
    };
    let cache = Path::new("../.cache/packages");
    release
        .extract(
            cache,
            out,
            &["BOOTAA64.EFI", "BOOTX64.EFI", "BOOTRISCV64.EFI"]
                .map(|name| format!("limine-binary/{name}").into()),
        )
        .unwrap();
    protocol
        .extract(
            cache,
            Path::new("../.cache/limine"),
            &[format!("limine-protocol-{rev}/include/limine.h").into()],
        )
        .unwrap();
}

fn build_port(arch: &str, flags: &[&str], out: &Path) {
    let target = format!("{arch}-unknown-none-elf");
    let mut compiler = cc::Build::new();
    compiler
        .compiler("clang")
        .target(&target)
        .cargo_metadata(false)
        .no_default_flags(true)
        .flag(format!("--target={target}"))
        .std("c23")
        .flags(["-Os", "-ffreestanding", "-fno-stack-protector", "-fno-pic"])
        .flags(flags.iter().copied())
        .include("include")
        .include("../.cache/limine")
        .include("arch/bringup")
        .files(["init/limine.c", "init/event.c", "mm/pmm.c", "lib/mem.c"])
        .files(files(Path::new("arch/bringup"), &["c", "S"]))
        .include(".")
        .files(files(&Path::new("arch").join(arch), &["c", "S"]))
        .out_dir(out.join(arch));
    if arch == "riscv64" {
        compiler.file("drivers/virtio_console.c");
    }
    let objects = compiler.compile_intermediates();
    let rust = out.join(arch).join("libkernel.a");
    let target = if arch == "riscv64" {
        "riscv64gc-unknown-none-elf"
    } else {
        "x86_64-unknown-none"
    };
    rust_archive(Path::new("lib.rs"), &rust, Some(target));
    run(Command::new("ld.lld")
        .args([
            "-static",
            "-z",
            "max-page-size=4096",
            "-T",
            "arch/bringup/linker.ld",
        ])
        .arg("--gc-sections")
        .args(objects)
        .arg(rust)
        .arg("-o")
        .arg(out.join(format!("{arch}.elf"))));
}

fn main() {
    println!("cargo:rerun-if-changed=.");
    let out = output_dir();
    limine(&out);
    for (arch, flags) in [
        (
            "x86_64",
            &["-mcmodel=kernel", "-mno-red-zone", "-mgeneral-regs-only"][..],
        ),
        (
            "riscv64",
            &[
                "-march=rv64gc",
                "-mabi=lp64d",
                "-mcmodel=medany",
                "-mno-relax",
            ][..],
        ),
    ] {
        std::fs::create_dir_all(out.join(arch)).unwrap();
        build_port(arch, flags, &out);
    }
    let mut clang = cc::Build::new();
    clang
        .compiler("clang")
        .target("aarch64-unknown-none-softfloat")
        .no_default_flags(true)
        .cargo_metadata(false)
        .flag("-O2")
        .flag("-g")
        .define(
            "SPORE_VERSION",
            format!("\"{}\"", env!("CARGO_PKG_VERSION")).as_str(),
        );
    for flag in include_str!("compile_flags.txt").lines() {
        clang.flag(flag);
    }
    if env::var_os("CARGO_FEATURE_USERLAND").is_some() {
        clang.define("SPORE_USERLAND", None);
    }
    let objects = clang
        .files(
            [
                "arch/aarch64",
                "abi",
                "drivers",
                "exec",
                "fs",
                "init",
                "lib",
                "mm",
                "net",
                "proc",
                "tty",
            ]
            .into_iter()
            .flat_map(|dir| files(Path::new(dir), &["c", "S"])),
        )
        .compile_intermediates();
    let rust = out.join("libkernel.a");
    rust_archive(
        Path::new("lib.rs"),
        &rust,
        Some("aarch64-unknown-none-softfloat"),
    );
    let elf = out.join("kernel.elf");
    run(Command::new("ld.lld")
        .args([
            "-static",
            "-z",
            "max-page-size=0x1000",
            "-T",
            "arch/aarch64/linker.ld",
        ])
        .args(objects)
        .arg(rust)
        .arg("-o")
        .arg(&elf));
    for (flags, name) in [
        (&["--strip-debug"][..], "kernel-boot.elf"),
        (&["-O", "binary"][..], "spore.bin"),
    ] {
        run(Command::new("llvm-objcopy")
            .args(flags)
            .arg(&elf)
            .arg(out.join(name)));
    }
}
