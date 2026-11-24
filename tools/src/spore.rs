mod app_network;
mod image;
mod runner;
mod system;

use std::{
    env,
    ffi::OsString,
    fs, io,
    os::unix::fs::DirBuilderExt,
    path::{Path, PathBuf},
    process::ExitCode,
};

const HELP: &str = "\
Usage:
  spore boot [--snapshot | --reset] [--root EXT2] [VM options]
  spore boot SYSTEM_IMAGE --root EXT2 [--snapshot] [VM options]
  spore build                         Build the kernel and host CLI
  spore build --userland [--tests]    Enable and build the full system or integration image
  spore build ELF_OR_PACKAGE -o IMAGE [image options] [-- ARG...]
  spore run ELF_OR_PACKAGE [image options] [VM options] [-- ARG...]
  spore boot APP_IMAGE [VM options]
  spore test [host|guest|shell|appliance|rng]
  spore bench [guest|inodes|appliance]

Image options:
  --exec PATH        Guest executable path (default /app/server)
  --argv0 NAME       Override argv[0]
  --cwd PATH         Working directory (default /)
  --env NAME=VALUE   Environment entry, repeatable
  --file HOST=/PATH  Include an asset, repeatable
  --lib-dir DIR      Search for AArch64 libraries, repeatable
  --ip IP --netmask MASK --gateway IP --dns IP

VM options:
  --memory SIZE      Guest memory (app 128M, full system 8G)
  --smp N            Virtual CPUs (app 1, full system 4)
  --qemu PATH        QEMU executable (also configurable with QEMU)
  --accel ACCEL      QEMU accelerator
  --cpu MODEL        QEMU CPU model
  --vars FD          Firmware variable store
  --timings          Print full-system boot timings
  -p PORT, --forward HOST:GUEST    Forward TCP on localhost (repeatable)
  --data EXT2        Mount a writable disk at /data
  --result JSON      Write workload status and timing
  --boot-timeout S   Seconds to wait for app entry (default 30)";

struct Workspace(PathBuf);
impl Workspace {
    fn new() -> io::Result<Self> {
        Self::at(&env::temp_dir())
    }
    fn at(base: &Path) -> io::Result<Self> {
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

fn qemu_default() -> OsString {
    env::var_os("QEMU").unwrap_or_else(|| {
        let brew = "/opt/homebrew/bin/qemu-system-aarch64";
        if cfg!(target_os = "macos") && Path::new(brew).exists() {
            brew.into()
        } else {
            "qemu-system-aarch64".into()
        }
    })
}

fn vm_config(mode: &str) -> runner::qemu::Config {
    runner::qemu::Config {
        mode: mode.into(),
        image: OsString::new(),
        root: None,
        vars: None,
        control: None,
        report: None,
        forwards: vec![],
        timeout: 30,
        qemu: qemu_default(),
        accel: None,
        cpu: None,
        mem: if mode == "app" { "128M" } else { "8G" }.into(),
        smp: if mode == "app" { "1" } else { "4" }.into(),
        timings: false,
        snap: false,
    }
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
    if action == "_image" {
        image::generate(&args.map(PathBuf::from).collect::<Vec<_>>())?;
        return Ok(0);
    }
    let check = action == "test" || action == "bench";
    if !build && !boot && !check && action != "run" {
        return Err(io::Error::other("expected boot, run, build, test or bench"));
    }
    let mut program = None;
    let mut output = None;
    let mut pack = Vec::new();
    let exe = env::current_exe()?;
    let artifacts = exe
        .parent()
        .ok_or_else(|| io::Error::other("missing executable directory"))?;
    if check {
        let suite = args.next();
        if suite.as_deref().is_some_and(|s| s == "-h" || s == "--help") {
            println!("{HELP}");
            return Ok(0);
        }
        if args.next().is_some() {
            return Err(io::Error::other("expected a single suite name"));
        }
        return system::check(artifacts, action == "bench", suite.as_deref());
    }
    let mut vm = vm_config("app");
    let mut memory = None;
    let mut cpus = None;
    let mut root = None;
    let mut reset = false;
    let mut userland = false;
    let mut tests = false;
    let mut app_options = false;
    while let Some(key) = args.next() {
        if key == "-h" || key == "--help" {
            println!("{HELP}");
            return Ok(0);
        }
        if build && key == "--userland" {
            userland = true;
            continue;
        }
        if build && key == "--tests" {
            tests = true;
            continue;
        }
        if boot && key == "--reset" {
            reset = true;
            continue;
        }
        if boot && key == "--snapshot" {
            vm.snap = true;
            continue;
        }
        if !build && key == "--timings" {
            vm.timings = true;
            continue;
        }
        if key == "--" && !boot {
            pack.push(PathBuf::from(key));
            pack.extend(args.map(PathBuf::from));
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
        if matches!(key.to_str(), Some("--boot-timeout" | "--result" | "--data")) {
            app_options = true;
        }
        match key.to_str() {
            Some("-o" | "--output") if build => output = Some(PathBuf::from(value)),
            Some(
                "--exec" | "--argv0" | "--cwd" | "--env" | "--file" | "--lib-dir" | "--ip"
                | "--gateway" | "--dns" | "--netmask",
            ) if !boot => pack.extend([PathBuf::from(key), PathBuf::from(value)]),
            Some("--memory") if !build => memory = Some(value),
            Some("--root") if boot => root = Some(value),
            Some("--accel") if !build => vm.accel = Some(value),
            Some("--cpu") if !build => vm.cpu = Some(value),
            Some("--vars") if !build => vm.vars = Some(value),
            Some("--qemu") if !build => vm.qemu = value,
            Some("--smp") if !build => {
                positive::<u32>(&value, "CPU count")?;
                cpus = Some(value);
            }
            Some("--boot-timeout") if !build => vm.timeout = positive(&value, "boot timeout")?,
            Some("--result") if !build => vm.report = Some(value),
            Some("--data") if !build => vm.root = Some(value),
            Some("-p" | "--forward") if !build => {
                let ports = value
                    .to_str()
                    .ok_or_else(|| io::Error::other("expected PORT or HOST:GUEST"))?;
                let (h, g) = ports.split_once(':').unwrap_or((ports, ports));
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
    let system = boot && (program.is_none() || root.is_some());
    if system {
        vm.mem = "8G".into();
        vm.smp = "4".into();
    }
    if let Some(memory) = memory {
        vm.mem = memory;
    }
    if let Some(cpus) = cpus {
        vm.smp = cpus;
    }
    if system {
        if app_options {
            return Err(io::Error::other(
                "--data, --result and --boot-timeout require an app",
            ));
        }
        vm.mode = "plain".into();
        vm.root = root;
        return system::boot(artifacts, vm, program, reset);
    }
    if reset || vm.snap || vm.timings {
        return Err(io::Error::other(
            "--reset, --snapshot and --timings require full-system boot",
        ));
    }
    if build && program.is_none() && pack.is_empty() && output.is_none() {
        if userland {
            system::enable_userland(artifacts)?;
        } else if tests {
            system::require_userland()?;
        }
        let target = if tests {
            "test_image.img"
        } else if userland {
            "image.img"
        } else {
            "cli"
        };
        system::compile(artifacts, Path::new(target))?;
        return Ok(0);
    }
    if tests || userland {
        return Err(io::Error::other(
            "--userland and --tests apply to full-system builds",
        ));
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
        let result = system::program(artifacts, &input)
            .and_then(|program| image::app::build(artifacts, &image, &program, &pack));
        if let Err(e) = result {
            eprintln!("spore: {e}");
            return Ok(1);
        }
        image
    };
    if build {
        return Ok(0);
    }
    vm.image = image.into_os_string();
    vm.control = Some(tmp.0.join("control").into_os_string());
    vm.vars
        .get_or_insert_with(|| tmp.0.join("vars.fd").into_os_string());
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
