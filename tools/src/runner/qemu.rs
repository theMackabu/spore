use crate::app_network;
use std::{
    ffi::{OsStr, OsString},
    fs, io,
    os::unix::ffi::OsStrExt,
    os::unix::fs::{FileExt, MetadataExt},
    path::Path,
    process::Command,
};
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

fn default_accelerator() -> &'static str {
    if cfg!(all(target_arch = "aarch64", target_os = "macos")) {
        "hvf"
    } else if cfg!(all(target_arch = "aarch64", target_os = "linux"))
        && fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/kvm")
            .is_ok()
    {
        "kvm"
    } else {
        "tcg"
    }
}

pub struct DirectBoot {
    pub kernel: std::path::PathBuf,
    pub ram: u64,
    pub network: [std::net::Ipv4Addr; 4],
}

impl DirectBoot {
    pub fn memory(config: &Config) -> Option<u64> {
        if config.smp != "1" || config.vars.is_some() {
            return None;
        }
        let digits = config.mem.to_str()?.strip_suffix('M')?;
        if digits.starts_with('0') || !digits.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        let mib: u64 = digits.parse().ok()?;
        ((32..=1024).contains(&mib) && mib.is_multiple_of(2)).then(|| mib << 20)
    }

    fn append(&self, args: &mut Vec<OsString>, bundle: &OsStr) -> io::Result<()> {
        let size = fs::metadata(bundle)?.len();
        if size > self.ram - 0x400000 {
            return Err(io::Error::other("app payload exceeds guest RAM"));
        }
        let epoch = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(io::Error::other)?
            .as_secs();
        args.extend([
            "-device".into(),
            drive(
                "loader,file=",
                self.kernel.as_os_str(),
                ",addr=0x40200000,cpu-num=0,force-raw=on",
            ),
            "-device".into(),
            drive("loader,file=", bundle, ",addr=0x40400000,force-raw=on"),
        ]);
        for (address, word) in [
            (0x403fffe8, self.ram),
            (0x403ffff0, size),
            (0x403ffff8, epoch),
        ] {
            args.extend([
                "-device".into(),
                format!("loader,addr={address:#x},data={word:#x},data-len=8").into(),
            ]);
        }
        Ok(())
    }
}

pub struct Config {
    pub direct: Option<DirectBoot>,
    pub mode: String,
    pub image: OsString,
    pub root: Option<OsString>,
    pub vars: Option<OsString>,
    pub report: Option<OsString>,
    pub forwards: Vec<(u16, u16)>,
    pub timeout: u64,
    pub qemu: OsString,
    pub accel: Option<OsString>,
    pub cpu: Option<OsString>,
    pub mem: OsString,
    pub smp: OsString,
    pub timings: bool,
    pub snap: bool,
}

fn network_manifest(image: &OsStr) -> io::Result<[std::net::Ipv4Addr; 4]> {
    let output = Command::new("mtype")
        .arg("-i")
        .arg(image)
        .arg("::/limine.conf")
        .output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "cannot read Limine configuration from {}",
            Path::new(image).display()
        )));
    }
    let config = String::from_utf8(output.stdout).map_err(io::Error::other)?;
    let fields = config
        .lines()
        .find_map(|line| line.strip_prefix("# spore-ipv4 "))
        .ok_or_else(|| io::Error::other("image does not contain an app network manifest"))?;
    let ips = fields
        .split_whitespace()
        .map(str::parse)
        .collect::<Result<Vec<_>, _>>()
        .map_err(io::Error::other)?;
    ips.try_into()
        .map_err(|_| io::Error::other("invalid app network manifest"))
}

pub fn run(config: Config) -> io::Result<i32> {
    if let Some(target) = config.report.as_ref().and_then(|p| fs::metadata(p).ok()) {
        for input in [
            Some(config.image.as_os_str()),
            config.root.as_deref(),
            config.vars.as_deref(),
            config.direct.as_ref().map(|boot| boot.kernel.as_os_str()),
        ]
        .into_iter()
        .flatten()
        {
            if let Ok(src) = fs::metadata(input)
                && src.dev() == target.dev()
                && src.ino() == target.ino()
            {
                return Err(io::Error::other("result path aliases a VM input"));
            }
        }
    }
    let report = config.report.clone();
    let app = config.mode == "app";
    let result = launch(config);
    if result.is_err() && app {
        super::app::report(report.as_deref().map(Path::new), "host-error", 125, None)?;
    }
    result
}

fn launch(config: Config) -> io::Result<i32> {
    let Config {
        direct,
        mode,
        image,
        root,
        vars,
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
    } = config;
    let mode = mode.as_str();
    let (mut net, guest) = if mode == "app" {
        if let Some(path) = &root {
            let mut magic = [0; 2];
            fs::File::open(path)?.read_exact_at(&mut magic, 1024 + 56)?;
            if magic != [0x53, 0xef] {
                return Err(io::Error::other("data volume requires an ext2 filesystem"));
            }
        }
        let cfg = match &direct {
            Some(boot) => boot.network,
            None => network_manifest(&image)?,
        };
        (app_network::user_network(cfg)?, cfg[0].to_string())
    } else {
        ("user,id=sporenet".into(), String::new())
    };
    for (host, port) in forwards {
        net.push_str(&format!(",hostfwd=tcp:127.0.0.1:{host}-{guest}:{port}"));
    }
    let accel = accel.unwrap_or_else(|| default_accelerator().into());
    let cpu = cpu.unwrap_or_else(|| {
        if accel.as_bytes().split(|&b| b == b',').next() == Some(b"tcg") {
            "max".into()
        } else {
            "host".into()
        }
    });
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
        net.into(),
        "-device".into(),
        "virtio-net-device,netdev=sporenet,mac=52:54:00:12:34:56".into(),
    ];
    if let Some(boot) = direct {
        boot.append(&mut q, &image)?;
    } else {
        append_firmware(&mut q, &image, vars.as_deref())?;
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
        "-chardev".into(),
        "stdio,id=serial0,signal=off".into(),
        "-serial".into(),
        "chardev:serial0".into(),
        "-display".into(),
        "none".into(),
    ]);
    if mode == "app" {
        return super::app::run(q, report.as_deref().map(Path::new), timeout);
    }
    super::fixtures::start(mode)?;
    super::harness::run(&q, mode, timings)
}

fn append_firmware(q: &mut Vec<OsString>, image: &OsStr, vars: Option<&OsStr>) -> io::Result<()> {
    let dirs = firmware_dirs(&q[0]);
    let Some(firmware) = find(&dirs, &["edk2-aarch64-code.fd"]) else {
        return Err(io::Error::other("edk2-aarch64-code.fd not found"));
    };

    let template = || {
        find(&dirs, &["edk2-arm-vars.fd", "edk2-aarch64-vars.fd"])
            .ok_or_else(|| io::Error::other("edk2 vars template not found"))
    };
    let vars_drive = if let Some(v) = vars {
        let path = Path::new(v);
        if !path.try_exists()? {
            let stage = crate::Workspace::at(path.parent().unwrap_or(Path::new(".")))?;
            let copy = stage.0.join("vars.fd");
            fs::copy(template()?, &copy)?;
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&copy, fs::Permissions::from_mode(0o600))?;
            if let Err(e) = fs::hard_link(&copy, path)
                && e.kind() != io::ErrorKind::AlreadyExists
            {
                return Err(e);
            }
        }
        drive("if=pflash,format=raw,file=", v, "")
    } else {
        drive(
            "if=pflash,format=raw,snapshot=on,file=",
            template()?.as_os_str(),
            "",
        )
    };
    q.extend([
        "-boot".into(),
        "order=d,menu=on,splash-time=0,strict=on".into(),
        "-drive".into(),
        drive(
            "if=pflash,format=raw,readonly=on,file=",
            firmware.as_os_str(),
            "",
        ),
    ]);
    q.extend(["-drive".into(), vars_drive]);
    if image.as_bytes().ends_with(b".iso") {
        q.extend(["-cdrom".into(), image.to_owned()])
    } else {
        q.extend([
            "-drive".into(),
            drive(
                "if=none,format=raw,readonly=on,file=",
                image,
                ",id=sporeesp",
            ),
            "-device".into(),
            "virtio-blk-device,drive=sporeesp,bootindex=0".into(),
        ])
    }
    Ok(())
}
