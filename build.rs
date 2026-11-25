fn main() {
    let mut ext2 = cc::Build::new();
    ext2.no_default_flags(true)
        .flag("-fPIC")
        .flag("-O2")
        .std("c23")
        .include("kernel/include");
    for dir in [
        "kernel/fs/ext2",
        "kernel/include/fs",
        "kernel/include/lib",
        "kernel/include/mm",
    ] {
        println!("cargo:rerun-if-changed={dir}");
    }
    for file in std::fs::read_dir("kernel/fs/ext2").unwrap() {
        let path = file.unwrap().path();
        if path.extension().is_some_and(|s| s == "c") {
            ext2.file(path);
        }
    }
    for src in [
        "kernel/mm/table.c",
        "kernel/lib/mem.c",
        "tools/src/storage/ext2.c",
    ] {
        println!("cargo:rerun-if-changed={src}");
        ext2.file(src);
    }
    ext2.compile("spore_ext2");
    println!("cargo:rerun-if-changed=tools/src/runner/terminal.c");
    cc::Build::new()
        .no_default_flags(true)
        .flag("-fPIC")
        .file("tools/src/runner/terminal.c")
        .std("c23")
        .compile("terminal");
}
