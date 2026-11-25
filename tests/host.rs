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

#[test]
fn userland_upgrade() {
    assert!(
        Command::new("sh")
            .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/upgrade.sh"))
            .arg(env!("CARGO_BIN_EXE_spore"))
            .status()
            .unwrap()
            .success()
    );
}

#[path = "../tools/src/runner/terminal_output.rs"]
mod terminal_output;

#[test]
fn fragmented_terminal_redraw() {
    let redraw = "\x1b[38;5;174m──❯🙂\x1b[0m\r\x1b[2K";
    for width in 1..=redraw.len() {
        let mut screen = terminal_output::TerminalOutput::default();
        let mut received = Vec::new();
        for chunk in redraw.as_bytes().chunks(width) {
            let mut emitted = Vec::new();
            screen.relay(&mut emitted, chunk).unwrap();
            assert!(std::str::from_utf8(&emitted).is_ok());
            if let Some(esc) = emitted.iter().rposition(|&b| b == 0x1b) {
                assert!(emitted[esc..].starts_with(b"\x1b["));
                assert!(emitted[esc + 2..].iter().any(|b| (0x40..=0x7e).contains(b)));
            }
            received.extend(emitted);
        }
        screen.finish(&mut received).unwrap();
        assert_eq!(received, redraw.as_bytes());
    }
}
