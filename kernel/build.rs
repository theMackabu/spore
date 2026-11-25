use spore_build::{files, output_dir, run, rust_archive};
use spore_packages::Archive;
use std::{env, path::Path, process::Command, time::SystemTime};

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

fn abi_sources(arch: &str) -> impl Iterator<Item = std::path::PathBuf> {
    ["linux", "freebsd"]
        .into_iter()
        .flat_map(move |os| files(&Path::new("abi").join(os).join(arch), &["c", "S"]))
}

fn runtime_sources() -> impl Iterator<Item = std::path::PathBuf> {
    [
        "io_uring",
        "mm",
        "fs",
        "proc",
        "net",
        "tty",
        "lib",
        "exec",
        "abi/linux/shared",
    ]
    .into_iter()
    .flat_map(|dir| files(Path::new(dir), &["c", "S"]))
}

fn build_port(arch: &str, flags: &[&str], out: &Path, epoch: &str) {
    let target = format!("{arch}-unknown-none-elf");
    let mut compiler = cc::Build::new();
    compiler
        .env("SOURCE_DATE_EPOCH", epoch)
        .compiler("clang")
        .target(&target)
        .cargo_metadata(false)
        .no_default_flags(true)
        .flag(format!("--target={target}"))
        .std("c23")
        .define("SPORE_NATIVE_PORT", None)
        .flags([
            "-Os",
            "-ffreestanding",
            "-fno-stack-protector",
            "-fno-pic",
            "-ffunction-sections",
            "-fdata-sections",
        ])
        .define("SPORE_MACHINE", format!("\"{arch}\"").as_str())
        .define(
            "SPORE_VERSION",
            format!("\"{}\"", env!("CARGO_PKG_VERSION")).as_str(),
        )
        .flags(flags.iter().copied())
        .include("include")
        .include("../.cache/limine")
        .include("arch/bringup")
        .files([
            "init/limine.c",
            "init/event.c",
            "init/process.c",
            "init/profile.c",
            "init/payload.c",
            "init/rootfs.c",
            "init/status.c",
            "abi/freebsd/dispatch.c",
            "drivers/virtio_console.c",
            "drivers/virtio_mmio.c",
            "drivers/virtio_net.c",
            "drivers/virtio_blk.c",
        ])
        .file("arch/smp.c")
        .files(runtime_sources())
        .files(files(Path::new("arch/bringup"), &["c", "S"]))
        .files(abi_sources(arch))
        .include(".")
        .files(files(&Path::new("arch").join(arch), &["c", "S"]))
        .out_dir(out.join(arch));
    if env::var_os("CARGO_FEATURE_USERLAND").is_some() {
        compiler.define("SPORE_USERLAND", None);
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
        .arg("--error-limit=0")
        .arg("--gc-sections")
        .args(objects)
        .arg(rust)
        .arg("-o")
        .arg(out.join(format!("{arch}.elf"))));
}

fn main() {
    println!("cargo:rerun-if-changed=.");
    println!("cargo:rerun-if-env-changed=SOURCE_DATE_EPOCH");
    let epoch = env::var("SOURCE_DATE_EPOCH").unwrap_or_else(|_| {
        SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap()
            .as_secs()
            .to_string()
    });
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
        build_port(arch, flags, &out, &epoch);
    }
    let mut clang = cc::Build::new();
    clang
        .env("SOURCE_DATE_EPOCH", &epoch)
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
        .define("SPORE_MACHINE", "\"aarch64\"")
        .file("abi/freebsd/dispatch.c")
        .files(abi_sources("aarch64"))
        .file("arch/smp.c")
        .files(runtime_sources())
        .files(
            ["arch/aarch64", "drivers", "init"]
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
