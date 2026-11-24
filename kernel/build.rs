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
        .extract(cache, out, &["limine-binary/BOOTAA64.EFI".into()])
        .unwrap();
    protocol
        .extract(
            cache,
            Path::new("../.cache/limine"),
            &[format!("limine-protocol-{rev}/include/limine.h").into()],
        )
        .unwrap();
}

fn main() {
    println!("cargo:rerun-if-changed=.");
    let out = output_dir();
    limine(&out);
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
        .files(files(Path::new("."), &["c", "S"]))
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
