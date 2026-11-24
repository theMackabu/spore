mod app_network;
mod runner;

use std::{
    env,
    ffi::OsString,
    fs, io,
    os::unix::fs::DirBuilderExt,
    path::{Path, PathBuf},
    process::{Command, ExitCode},
};

const HELP: &str = "\
Usage:
  spore build ELF -o IMAGE [image options] [-- ARG...]
  spore run ELF [image options] [VM options] [-- ARG...]
  spore boot IMAGE [VM options]

Image options:
  --exec PATH        Guest executable path (default /app/server)
  --argv0 NAME       Override argv[0]
  --cwd PATH         Working directory (default /)
  --env NAME=VALUE   Environment entry, repeatable
  --file HOST=/PATH  Include an asset, repeatable
  --lib-dir DIR      Search for AArch64 libraries, repeatable
  --ip IP --netmask MASK --gateway IP --dns IP

VM options:
  --memory SIZE      Guest memory (default 128M)
  --smp N            Virtual CPUs (default 1)
  --forward H:G      Forward a localhost TCP port to a guest port
  --data EXT2        Mount a writable disk at /data
  --result JSON      Write workload status and timing
  --boot-timeout S   Seconds to wait for app entry (default 30)";

struct Workspace(PathBuf);
impl Workspace {
    fn new() -> io::Result<Self> {
        let base = env::temp_dir();
        for n in 0u32.. {
            let path = base.join(format!("spore-{}-{n}", std::process::id()));
            match fs::DirBuilder::new().mode(0o700).create(&path) {
                Ok(()) => return Ok(Self(path)),
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e),
            }
        }
        Err(io::Error::other("temporary directory unavailable"))
    }
}
impl Drop for Workspace {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn positive<T: std::str::FromStr + Default + PartialEq>(
    v: &std::ffi::OsStr,
    label: &str,
) -> io::Result<T> {
    v.to_str()
        .and_then(|s| s.parse::<T>().ok())
        .filter(|n| *n != T::default())
        .ok_or_else(|| io::Error::other(format!("invalid {label}: {}", v.to_string_lossy())))
}

fn execute() -> io::Result<i32> {
    let mut args = env::args_os().skip(1);
    let Some(action) = args.next() else {
        println!("{HELP}");
        return Ok(2);
    };
    if action == "--help" || action == "-h" || action == "help" {
        println!("{HELP}");
        return Ok(0);
    }
    let build = action == "build";
    let boot = action == "boot";
    if !build && !boot && action != "run" {
        return Err(io::Error::other("expected build, run, or boot"));
    }
    let mut program = None;
    let mut output = None;
    let mut pack = Vec::new();
    let exe = env::current_exe()?;
    let artifacts = exe
        .parent()
        .ok_or_else(|| io::Error::other("missing executable directory"))?;
    let qemu = env::var_os("QEMU").unwrap_or_else(|| {
        let brew = "/opt/homebrew/bin/qemu-system-aarch64";
        if cfg!(target_os = "macos") && Path::new(brew).exists() {
            brew.into()
        } else {
            "qemu-system-aarch64".into()
        }
    });
    let mut vm = runner::qemu::Config {
        mode: "app".into(),
        image: OsString::new(),
        root: None,
        vars: None,
        control: None,
        report: None,
        forwards: vec![],
        timeout: 30,
        qemu,
        accel: None,
        cpu: None,
        mem: "128M".into(),
        smp: "1".into(),
        timings: false,
        snap: false,
    };
    while let Some(key) = args.next() {
        if key == "-h" || key == "--help" {
            println!("{HELP}");
            return Ok(0);
        }
        if key == "--" && !boot {
            pack.push(key);
            pack.extend(args);
            break;
        }
        if !key.as_encoded_bytes().starts_with(b"-") {
            if program.replace(key).is_some() {
                return Err(io::Error::other(
                    "unexpected positional argument; pass guest arguments after --",
                ));
            }
            continue;
        }
        let value = args.next().ok_or_else(|| {
            io::Error::other(format!("missing value for {}", key.to_string_lossy()))
        })?;
        match key.to_str() {
            Some("-o" | "--output") if build => output = Some(PathBuf::from(value)),
            Some(
                "--exec" | "--argv0" | "--cwd" | "--env" | "--file" | "--lib-dir" | "--ip"
                | "--gateway" | "--dns" | "--netmask",
            ) if !boot => pack.extend([key, value]),
            Some("--memory") if !build => vm.mem = value,
            Some("--smp") if !build => {
                positive::<u32>(&value, "CPU count")?;
                vm.smp = value;
            }
            Some("--boot-timeout") if !build => vm.timeout = positive(&value, "boot timeout")?,
            Some("--result") if !build => vm.report = Some(value),
            Some("--data") if !build => vm.root = Some(value),
            Some("--forward") if !build => {
                let (h, g) = value
                    .to_str()
                    .and_then(|s| s.split_once(':'))
                    .ok_or_else(|| io::Error::other("expected HOST:GUEST port pair"))?;
                vm.forwards.push((
                    positive(h.as_ref(), "host port")?,
                    positive(g.as_ref(), "guest port")?,
                ));
            }
            _ => {
                return Err(io::Error::other(format!(
                    "unexpected option {}",
                    key.to_string_lossy()
                )));
            }
        }
    }
    let input = program.ok_or_else(|| io::Error::other("missing ELF or image path"))?;
    let tmp = Workspace::new()?;
    let image = if boot {
        PathBuf::from(input)
    } else {
        let image = if build {
            output.ok_or_else(|| io::Error::other("build requires -o IMAGE"))?
        } else {
            tmp.0.join("app.img")
        };
        let status = Command::new(artifacts.join("tools/spore-image"))
            .arg("app")
            .arg(artifacts.join("kernel/kernel-boot.elf"))
            .arg(artifacts.join("boot/BOOTAA64.EFI"))
            .arg(artifacts.join("boot/LIMINE-LICENSE"))
            .arg(&image)
            .arg(input)
            .args(pack)
            .status()?;
        if !status.success() {
            return Ok(status.code().unwrap_or(1));
        }
        image
    };
    if build {
        return Ok(0);
    }
    vm.image = image.into_os_string();
    vm.control = Some(tmp.0.join("control").into_os_string());
    vm.vars = Some(tmp.0.join("vars.fd").into_os_string());
    runner::qemu::run(vm)
}

fn main() -> ExitCode {
    match execute() {
        Ok(code) => ExitCode::from(code as u8),
        Err(e) => {
            eprintln!("spore: {e}");
            ExitCode::from(125)
        }
    }
}
