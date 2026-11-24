#[path = "runner/fixtures.rs"]
mod fixtures;
#[path = "runner/modes.rs"]
mod modes;
mod runner {
    pub mod harness;
    pub mod serial;
}
use std::{
    env,
    ffi::{OsStr, OsString},
    fs,
    os::unix::ffi::OsStrExt,
    path::Path,
    process::{self, Command},
};
fn usage() -> ! {
    eprintln!(
        "usage: spore-run --image IMAGE [--mode MODE] [--root ROOT] [--snapshot-root] [--qemu QEMU] [--accel ACCEL] [--cpu CPU] [--memory MEM] [--smp N] [--vars FD] [--timings]"
    );
    process::exit(2)
}
fn firmware_dirs(qemu: &OsStr) -> Vec<std::path::PathBuf> {
    let mut dirs = Vec::new();
    if let Ok(output) = Command::new(qemu).args(["-L", "help"]).output() {
        for line in String::from_utf8_lossy(&output.stdout).lines() {
            let dir = line.trim();
            if dir.is_empty() {
                continue;
            }
            dirs.push(dir.into());
            if let Some((base, _)) = dir.split_once("-firmware") {
                dirs.push(Path::new(base).join("qemu"));
            }
        }
    }
    dirs.push("/opt/homebrew/opt/qemu/share/qemu".into());
    dirs
}

fn find(dirs: &[std::path::PathBuf], names: &[&str]) -> Option<std::path::PathBuf> {
    dirs.iter()
        .flat_map(|d| names.iter().map(move |n| d.join(n)))
        .find(|p| fs::File::open(p).is_ok())
}

fn drive(prefix: &str, path: &OsStr, suffix: &str) -> OsString {
    let mut value = OsString::from(prefix);
    for (i, part) in path.as_bytes().split(|&b| b == b',').enumerate() {
        if i > 0 {
            value.push(",,");
        }
        value.push(OsStr::from_bytes(part));
    }
    value.push(suffix);
    value
}

fn main() {
    let mut mode = OsString::from("filter");
    let (mut image, mut root, mut vars) = (None, None, None);
    let mut qemu = OsString::from("qemu-system-aarch64");
    let mut accel = OsString::from("hvf");
    let mut cpu = OsString::from("host");
    let mut mem = OsString::from("8G");
    let mut smp = OsString::from("4");
    let (mut timings, mut snap) = (false, false);
    let mut args = env::args_os().skip(1);
    while let Some(key) = args.next() {
        let mut value = || args.next().unwrap_or_else(|| usage());
        match key.to_str().unwrap_or_else(|| usage()) {
            "--mode" => mode = value(),
            "--image" => image = Some(value()),
            "--qemu" => qemu = value(),
            "--root" => root = Some(value()),
            "--accel" => accel = value(),
            "--cpu" => cpu = value(),
            "--memory" => mem = value(),
            "--smp" => smp = value(),
            "--vars" => vars = Some(value()),
            "--timings" => timings = true,
            "--snapshot-root" => snap = true,
            _ => usage(),
        }
    }
    let mode = mode.to_str().unwrap_or_else(|| usage());
    if !matches!(
        mode,
        "plain" | "filter" | "integration" | "shell" | "stdin" | "bench" | "rng"
    ) || (snap && root.is_none())
    {
        usage();
    }
    let image = image.unwrap_or_else(|| usage());
    let dirs = firmware_dirs(&qemu);
    let Some(firmware) = find(&dirs, &["edk2-aarch64-code.fd"]) else {
        eprintln!("spore-run: edk2-aarch64-code.fd not found");
        process::exit(1);
    };

    if let Some(v) = &vars {
        if fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(v)
            .is_err()
        {
            let Some(template) = find(&dirs, &["edk2-arm-vars.fd", "edk2-aarch64-vars.fd"]) else {
                eprintln!("spore-run: edk2 vars template not found");
                process::exit(1);
            };
            let copy = fs::File::open(&template).and_then(|mut src| {
                let mut dst = fs::File::create(v)?;
                std::io::copy(&mut src, &mut dst)
            });
            if let Err(e) = copy {
                eprintln!("spore-run: {}: {e}", Path::new(v).display());
                process::exit(1);
            }
        }
    }
    let mut q = vec![
        qemu,
        "-M".into(),
        "virt,gic-version=3,virtualization=off".into(),
        "-accel".into(),
        accel,
        "-cpu".into(),
        cpu,
        "-m".into(),
        mem,
        "-smp".into(),
        smp,
        "-global".into(),
        "virtio-mmio.force-legacy=false".into(),
        "-netdev".into(),
        "user,id=sporenet".into(),
        "-device".into(),
        "virtio-net-device,netdev=sporenet,mac=52:54:00:12:34:56".into(),
        "-boot".into(),
        "order=d,menu=off,strict=on".into(),
        "-drive".into(),
        drive(
            "if=pflash,format=raw,readonly=on,file=",
            firmware.as_os_str(),
            "",
        ),
    ];
    if let Some(v) = vars {
        q.extend(["-drive".into(), drive("if=pflash,format=raw,file=", &v, "")])
    }
    if image.as_bytes().ends_with(b".iso") {
        q.extend(["-cdrom".into(), image])
    } else {
        q.extend([
            "-drive".into(),
            drive(
                "if=none,format=raw,readonly=on,file=",
                &image,
                ",id=sporeesp",
            ),
            "-device".into(),
            "virtio-blk-device,drive=sporeesp,bootindex=0".into(),
        ])
    }
    if let Some(r) = root {
        q.extend([
            "-drive".into(),
            drive(
                if snap {
                    "if=none,format=raw,id=sporeroot,snapshot=on,file="
                } else {
                    "if=none,format=raw,id=sporeroot,file="
                },
                &r,
                "",
            ),
            "-device".into(),
            "virtio-blk-device,drive=sporeroot".into(),
        ])
    }
    q.extend([
        "-serial".into(),
        "stdio".into(),
        "-display".into(),
        "none".into(),
    ]);
    process::exit(runner::harness::run(q, mode, timings));
}
