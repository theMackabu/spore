mod app_network;
mod arch;
mod image;
mod runner;
mod storage;
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
  spore upgrade [--from-base ORIGINAL_EXT2]   Upgrade the installed userland
  spore rollback                      Select the previous installation
  spore boot SYSTEM_IMAGE --root EXT2 [--snapshot] [VM options]
  spore boot --arch CPU [--root EXT2] [VM options]
  spore build                         Build the kernel and host CLI
  spore build --userland [--tests]    Enable and build the full system or integration image
  spore build ELF_OR_PACKAGE -o IMAGE [image options] [-- ARG...]
  spore build --wasm                  Build the hosted ARM64 kernel and interpreter
  spore build --wasm ELF -o PAYLOAD [image options] [-- ARG...]
  spore boot --wasm [--net] [--root EXT2]       Boot a disposable snapshot of a userland disk
  spore run --wasm ELF [--net | -p PORT] [image options] [--data EXT2] [-- ARG...]
  spore run ELF_OR_PACKAGE [image options] [VM options] [-- ARG...]
  spore boot APP_IMAGE [VM options]
  spore test [host|guest|shell|appliance|ports|wasm|wasm-guest|rng|stress]
  spore bench [guest|inodes|appliance|startup|uring|wasm]

Image options:
  --exec PATH        Guest executable path (default /app/server)
  --argv0 NAME       Override argv[0]
  --cwd PATH         Working directory (default /)
  --env NAME=VALUE   Environment entry, repeatable
  --file HOST=/PATH  Include an asset, repeatable
  --lib-dir DIR      Search for guest libraries, repeatable
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

fn qemu_default(arch: arch::Arch) -> OsString {
    env::var_os("QEMU").unwrap_or_else(|| {
        let brew = "/opt/homebrew/bin/qemu-system-aarch64";
        if arch == arch::Arch::Aarch64 && cfg!(target_os = "macos") && Path::new(brew).exists() {
            brew.into()
        } else {
            format!("qemu-system-{}", arch.name()).into()
        }
    })
}

fn vm_config(mode: &str) -> runner::qemu::Config {
    runner::qemu::Config {
        mode: mode.into(),
        direct: None,
        image: OsString::new(),
        root: None,
        root_qcow: false,
        vars: None,
        report: None,
        forwards: vec![],
        timeout: 30,
        arch: arch::Arch::Aarch64,
        qemu: None,
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
    let check = action == "test" || action == "bench";
    let exe = env::current_exe()?;
    let artifacts = exe.parent().unwrap();
    if action == "rollback" {
        if args.next().is_some() {
            return Err(io::Error::other("usage: spore rollback"));
        }
        storage::rollback(artifacts)?;
        return Ok(0);
    }
    if action == "upgrade" {
        let from = match args.next().as_deref() {
            None => None,
            Some(v) if v == "--help" || v == "-h" => {
                println!("{HELP}");
                return Ok(0);
            }
            Some(v) if v == "--from-base" => {
                Some(PathBuf::from(args.next().ok_or_else(|| {
                    io::Error::other("missing original base path")
                })?))
            }
            _ => {
                return Err(io::Error::other(
                    "usage: spore upgrade [--from-base ORIGINAL_EXT2]",
                ));
            }
        };
        if args.next().is_some() {
            return Err(io::Error::other("unexpected upgrade argument"));
        }
        storage::upgrade(artifacts, from.as_deref())?;
        return Ok(0);
    }
    if action == "_image" {
        let tests = args.next().as_deref() == Some("tests".as_ref());
        image::system_image(artifacts, tests)?;
        return Ok(0);
    }
    if !build && !boot && !check && action != "run" {
        return Err(io::Error::other(
            "expected boot, run, build, upgrade, rollback, test or bench",
        ));
    }
    let mut program = None;
    let mut output = None;
    let mut pack = Vec::new();
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
    let mut architecture = None;
    let mut qemu = None;
    let mut memory = None;
    let mut cpus = None;
    let mut root = None;
    let mut reset = false;
    let mut userland = false;
    let mut tests = false;
    let mut wasm = false;
    let mut net = false;
    let mut qemu_options = false;
    let mut app_options = false;
    while let Some(key) = args.next() {
        if key == "-h" || key == "--help" {
            println!("{HELP}");
            return Ok(0);
        }
        if key == "--net" && !build {
            net = true;
            continue;
        }
        if key == "--wasm" {
            wasm = true;
            continue;
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
            qemu_options = true;
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
            Some("-o" | "--output") if build => {
                output = Some(PathBuf::from(value));
                continue;
            }
            Some(
                "--exec" | "--argv0" | "--cwd" | "--env" | "--file" | "--lib-dir" | "--ip"
                | "--gateway" | "--dns" | "--netmask",
            ) if !boot => {
                pack.extend([PathBuf::from(key), PathBuf::from(value)]);
                continue;
            }
            Some("-p" | "--forward") if !build => {
                let ports = value
                    .to_str()
                    .ok_or_else(|| io::Error::other("expected PORT or HOST:GUEST"))?;
                let (h, g) = ports.split_once(':').unwrap_or((ports, ports));
                vm.forwards.push((
                    positive(h.as_ref(), "host port")?,
                    positive(g.as_ref(), "guest port")?,
                ));
                continue;
            }
            Some("--smp") if !build => {
                positive::<u32>(&value, "CPU count")?;
                cpus = Some(value);
                continue;
            }
            Some("--memory") if !build => {
                memory = Some(value);
                continue;
            }
            Some("--root") if boot => {
                root = Some(value);
                continue;
            }
            Some("--data") if !build => {
                vm.root = Some(value);
                continue;
            }
            _ => (),
        }
        qemu_options = true;
        match key.to_str() {
            Some("--accel") if !build => vm.accel = Some(value),
            Some("--cpu") if !build => vm.cpu = Some(value),
            Some("--vars") if !build => vm.vars = Some(value),
            Some("--qemu") if !build => qemu = Some(value),
            Some("--arch") if boot => architecture = Some(value),
            Some("--boot-timeout") if !build => vm.timeout = positive(&value, "boot timeout")?,
            Some("--result") if !build => vm.report = Some(value),
            _ => {
                return Err(io::Error::other(format!(
                    "unexpected option {}",
                    key.to_string_lossy()
                )));
            }
        }
    }
    if wasm {
        if boot {
            if qemu_options || app_options || reset || program.is_some() {
                return Err(io::Error::other(
                    "usage: spore boot --wasm [--net] [--root EXT2]",
                ));
            }
            let base = root
                .map(PathBuf::from)
                .unwrap_or_else(|| artifacts.join("root.ext2"));
            if !base.is_file() {
                return Err(io::Error::other(
                    "supply --root EXT2 or build the userland image with spore build --userland",
                ));
            }
            let network = (net || !vm.forwards.is_empty())
                .then(|| app_network::forwards("user,id=sporenet".into(), "", &vm.forwards));
            return system::run_wasm(
                artifacts,
                None,
                Some(base.as_os_str()),
                network.as_deref(),
                memory.as_deref(),
                cpus.as_deref(),
            );
        }
        if qemu_options || tests || userland {
            return Err(io::Error::other(
                "Wasm supports app files, arguments, environment, cwd and --data",
            ));
        }
        if build && program.is_none() && output.is_none() && pack.is_empty() {
            return system::build_wasm(artifacts);
        }
        let input = program.ok_or_else(|| io::Error::other("missing AArch64 ELF"))?;
        let elf = system::program(&input)?;
        if arch::Arch::executable(&elf)? != arch::Arch::Aarch64 {
            return Err(io::Error::other("Wasm requires an AArch64 ELF"));
        }
        let tmp = Workspace::new()?;
        let payload = if build {
            output.ok_or_else(|| io::Error::other("build requires -o PAYLOAD"))?
        } else {
            tmp.0.join("app.bin")
        };
        let basename = elf
            .file_name()
            .and_then(|s| s.to_str())
            .ok_or_else(|| io::Error::other("invalid ELF filename"))?;
        let mut options = vec![
            PathBuf::from("--exec"),
            PathBuf::from("/app").join(basename),
        ];
        options.extend(pack);
        let default_port = vm
            .forwards
            .first()
            .map(|p| p.1)
            .filter(|p| vm.forwards.iter().all(|f| f.1 == *p));
        let mut network = None;
        image::build_image(&payload, |out| {
            let cfg = image::app::payload(
                &elf,
                &options,
                default_port,
                &tmp.0,
                out,
                arch::Arch::Aarch64,
            )?;
            if net || !vm.forwards.is_empty() {
                network = Some(app_network::forwards(
                    app_network::user_network(cfg)?,
                    &cfg[0].to_string(),
                    &vm.forwards,
                ));
            }
            Ok(())
        })?;
        if build {
            return Ok(0);
        }
        return system::run_wasm(
            artifacts,
            Some(&payload),
            vm.root.as_deref(),
            network.as_deref(),
            memory.as_deref(),
            cpus.as_deref(),
        );
    }
    if net {
        return Err(io::Error::other("--net applies to Wasm guests"));
    }
    if let Some(arch) = architecture.as_deref() {
        vm.arch = arch::Arch::parse(arch.to_str().unwrap_or(""))?;
    }
    if let Some(qemu) = &qemu {
        vm.qemu = Some(qemu.clone());
    }
    if action == "run" {
        runner::qemu::check_report(vm.report.as_deref(), &[program.as_deref()])?;
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
        if program.is_none()
            && architecture.is_some()
            && (vm.root.is_some() || vm.arch != arch::Arch::Aarch64)
        {
            if reset {
                return Err(io::Error::other(
                    "--reset requires a default persistent root disk",
                ));
            }
            if vm.root.is_some() {
                system::userland()?;
            }
            let tmp = Workspace::new()?;
            let image = tmp.0.join("boot.img");
            let config = tmp.0.join("limine.conf");
            fs::write(&config, include_bytes!("../../boot/limine.conf"))?;
            image::build_esp(&config, &image, None, vm.arch)?;
            return system::boot(artifacts, vm, Some(image.into()), false);
        }
        return system::boot(artifacts, vm, program, reset);
    }
    if reset || vm.snap || vm.timings {
        return Err(io::Error::other(
            "--reset, --snapshot and --timings require full-system boot",
        ));
    }
    if build && program.is_none() && pack.is_empty() && output.is_none() {
        return system::build_project(artifacts, userland, tests);
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
        let default_port = vm
            .forwards
            .first()
            .map(|&(_, port)| port)
            .filter(|port| vm.forwards.iter().all(|(_, guest)| guest == port));
        let result = system::program(&input).and_then(|program| {
            vm.arch = arch::Arch::executable(&program)?;
            if !build
                && vm.arch == arch::Arch::Aarch64
                && let Some(ram) = runner::qemu::DirectBoot::memory(&vm)
            {
                let kernel = tmp.0.join("spore.bin");
                fs::write(&kernel, spore_kernel::DIRECT)?;
                let network =
                    image::app::payload(&program, &pack, default_port, &tmp.0, &image, vm.arch)?;
                vm.direct = Some(runner::qemu::DirectBoot {
                    kernel,
                    ram,
                    network,
                });
                Ok(())
            } else {
                image::app::build(&image, &program, &pack, default_port, vm.arch)
            }
        });
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
