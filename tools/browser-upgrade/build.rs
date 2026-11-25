use std::path::Path;

fn main() {
    if std::env::var("TARGET").unwrap() != "wasm32-unknown-unknown" {
        return;
    }
    let kernel = Path::new("../../kernel");
    let mut c = cc::Build::new();
    c.compiler("clang")
        .archiver("llvm-ar")
        .no_default_flags(true)
        .std("c23")
        .flags(["--target=wasm32-unknown-unknown", "-ffreestanding", "-O2"])
        .include(kernel.join("include"));
    for entry in std::fs::read_dir(kernel.join("fs/ext2")).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().is_some_and(|ext| ext == "c") {
            c.file(path);
        }
    }
    c.file(kernel.join("lib/mem.c"))
        .define("SPORE_DISK_TOOL", None)
        .file("../src/storage/ext2.c")
        .compile("upgrade_ext2");
    for path in [
        "../../kernel/include",
        "../../kernel/fs/ext2",
        "../../kernel/lib/mem.c",
        "../src/storage/ext2.c",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
}
