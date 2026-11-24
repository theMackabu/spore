fn main() {
    println!("cargo:rerun-if-changed=tools/src/runner/terminal.c");
    cc::Build::new()
        .no_default_flags(true)
        .flag("-fPIC")
        .file("tools/src/runner/terminal.c")
        .std("c23")
        .compile("terminal");
}
