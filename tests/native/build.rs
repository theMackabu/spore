use spore_build::*;
use std::{env, path::Path};

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap();
    env::set_current_dir(root).unwrap();
    let out = output_dir();

    for dir in ["kernel", "tests"] {
        watch(&root.join(dir));
    }
    for var in ["CC", "AR", "CFLAGS", "LDFLAGS"] {
        println!("cargo:rerun-if-env-changed={var}");
    }
    let mut compiler = cc::Build::new();
    compiler
        .no_default_flags(true)
        .flag("-O2")
        .std("c23")
        .include("kernel")
        .include("kernel/include")
        .cargo_metadata(false);
    let mut sources = files(Path::new("kernel/fs/ext2"), &["c"]);
    sources.extend(files(Path::new("kernel/fs/ramfs"), &["c"]));
    sources.extend(
        [
            "lib/mem",
            "mm/table",
            "mm/pmm",
            "mm/pin",
            "mm/pool",
            "mm/page_ring",
            "mm/vma",
            "mm/vmm",
            "mm/file_page",
            "exec/stack",
            "exec/elf",
            "arch/aarch64/mmu",
            "net/net",
            "net/socket/tcp/transmit",
            "net/socket/tcp/rto",
            "net/socket/tcp/handshake",
            "net/socket/tcp/receive",
            "net/socket/tcp/reorder",
            "fs/vfs/page_cache",
        ]
        .map(|name| Path::new("kernel").join(name).with_extension("c")),
    );
    compiler.files(sources).compile("host");
    let lib = out.join("libhost.a");
    let rust = out.join("libhost_kernel.a");
    rust_archive(Path::new("kernel/lib.rs"), &rust, None);
    let tests: Vec<_> = files(Path::new("tests"), &["c"])
        .into_iter()
        .filter(|p| {
            p.parent() == Some(Path::new("tests"))
                && p.file_stem()
                    .unwrap()
                    .to_str()
                    .is_some_and(|n| n.starts_with("test_") || n == "bench_ext2_inodes")
        })
        .collect();
    for src in &tests {
        let name = src.file_stem().unwrap();
        let mut cmd = compiler.get_compiler().to_command();
        cmd.arg(src);
        match name.to_str().unwrap() {
            "test_stack" => {
                cmd.arg("tests/random_stub.c");
            }
            "test_elf" | "test_vma" => {
                cmd.arg("tests/vfs_ref_stub.c");
            }
            _ => (),
        }
        if let Ok(flags) = env::var("LDFLAGS") {
            cmd.args(flags.split_whitespace());
        }
        run(cmd.arg(&lib).arg(&rust).arg("-o").arg(out.join(name)));
    }
}
