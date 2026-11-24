use std::{env, path::Path, process::Command};

fn fixture(name: &str) {
    let mut cmd = Command::new(Path::new(spore_native_tests::ARTIFACTS).join(name));
    if name == "test_ext2"
        && let Some(disk) = env::var_os("SPORE_TEST_ROOT")
    {
        cmd.arg(disk);
    }
    assert!(cmd.status().unwrap().success(), "{name} failed");
}

macro_rules! native_tests {
    ($($name:ident),*) => { $(#[test] fn $name() { fixture(concat!("test_",stringify!($name))); })* };
}
native_tests!(
    pmm,
    stack,
    elf,
    ramfs,
    ext2,
    vmm,
    vma,
    abi,
    thread_model,
    futex_model,
    ipc_model,
    mycelium_model,
    net,
    tcp_stream,
    page_ring,
    page_cache
);

#[test]
#[ignore = "benchmark"]
fn bench_inodes() {
    fixture("bench_ext2_inodes");
}
