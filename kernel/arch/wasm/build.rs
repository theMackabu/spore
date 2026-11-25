use std::path::Path;

fn main() {
    if std::env::var("TARGET").unwrap() != "wasm32-unknown-unknown" {
        return;
    }
    let kernel = Path::new("../..");
    let mut cc = cc::Build::new();
    cc.env("SOURCE_DATE_EPOCH", spore_build::build_epoch())
        .compiler("clang")
        .archiver("llvm-ar")
        .no_default_flags(true)
        .flag("--target=wasm32-unknown-unknown")
        .std("c23")
        .flags([
            "-O2",
            "-ffreestanding",
            "-ffunction-sections",
            "-fdata-sections",
        ])
        .include(kernel)
        .include(kernel.join("include"))
        .define("SPORE_MACHINE", "\"aarch64\"")
        .define(
            "SPORE_VERSION",
            format!("\"{}\"", env!("CARGO_PKG_VERSION")).as_str(),
        );
    cc.files(spore_build::kernel_runtime_sources(kernel));
    for file in [
        "abi/freebsd/dispatch.c",
        "abi/freebsd/aarch64/syscall.c",
        "init/process.c",
        "init/payload.c",
        "arch/aarch64/signal_frame.c",
        "arch/aarch64/frame.c",
        "arch/wasm/platform.c",
    ] {
        let path = kernel.join(file);
        println!("cargo:rerun-if-changed={}", path.display());
        cc.file(path);
    }
    for dir in ["include", "init"] {
        println!("cargo:rerun-if-changed={}", kernel.join(dir).display());
    }
    cc.compile("wasm_kernel");
    println!("cargo:rustc-link-arg=--fatal-warnings");
}
