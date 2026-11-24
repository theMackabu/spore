mod app_network;
mod runner;
use std::{env, ffi::OsString, process};
fn usage() -> ! {
    eprintln!(
        "usage: spore-run --image IMAGE [--mode MODE] [--root ROOT] [--snapshot-root] [--qemu QEMU] [--accel ACCEL] [--cpu CPU] [--memory MEM] [--smp N] [--vars FD] [--timings]"
    );
    process::exit(2)
}
fn main() {
    let mut mode = OsString::from("filter");
    let (mut image, mut root, mut vars) = (None, None, None);
    let (mut control, mut report) = (None, None);
    let mut forwards = Vec::new();
    let mut timeout = 30u64;
    let mut qemu = OsString::from("qemu-system-aarch64");
    let mut accel = None;
    let mut cpu = None;
    let mut mem = OsString::from("8G");
    let mut smp = OsString::from("4");
    let (mut timings, mut snap) = (false, false);
    let mut args = env::args_os().skip(1);
    while let Some(key) = args.next() {
        let mut value = || args.next().unwrap_or_else(|| usage());
        match key.to_str().unwrap_or_else(|| usage()) {
            "--control" => control = Some(value()),
            "--result" => report = Some(value()),
            "--boot-timeout" => {
                timeout = value()
                    .to_str()
                    .and_then(|s| s.parse::<u64>().ok())
                    .filter(|&n| n > 0)
                    .unwrap_or_else(|| usage())
            }
            "--forward" => {
                let v = value();
                let (h, g) = v
                    .to_str()
                    .and_then(|s| s.split_once(':'))
                    .unwrap_or_else(|| usage());
                let port = |p: &str| {
                    p.parse::<u16>()
                        .ok()
                        .filter(|&n| n != 0)
                        .unwrap_or_else(|| usage())
                };
                forwards.push((port(h), port(g)));
            }
            "--mode" => mode = value(),
            "--image" => image = Some(value()),
            "--qemu" => qemu = value(),
            "--root" => root = Some(value()),
            "--accel" => accel = Some(value()),
            "--cpu" => cpu = Some(value()),
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
        "app" | "plain" | "filter" | "integration" | "shell" | "stdin" | "bench" | "rng"
    ) || (snap && root.is_none())
        || ((mode == "app") != control.is_some())
    {
        usage();
    }
    let image = image.unwrap_or_else(|| usage());
    let config = runner::qemu::Config {
        mode: mode.into(),
        image,
        root,
        vars,
        control,
        report,
        forwards,
        timeout,
        qemu,
        accel,
        cpu,
        mem,
        smp,
        timings,
        snap,
    };
    let rc = runner::qemu::run(config).unwrap_or_else(|e| {
        eprintln!("spore-run: {e}");
        if mode == "app" { 125 } else { 1 }
    });
    process::exit(rc);
}
