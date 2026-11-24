use super::qemu::{drive, find, firmware_dirs};
use crate::Workspace;
use std::{
    env,
    ffi::OsStr,
    fs, io,
    io::Read,
    path::{Path, PathBuf},
    process::Command,
};

pub fn executable_arch(path: &Path) -> io::Result<Option<&'static str>> {
    let mut header = [0; 20];
    if let Err(e) = fs::File::open(path)?.read_exact(&mut header) {
        if e.kind() == io::ErrorKind::UnexpectedEof {
            return Ok(None);
        }
        return Err(e);
    }
    if &header[..7] != b"\x7fELF\x02\x01\x01" {
        return Ok(None);
    }
    Ok(match u16::from_le_bytes([header[18], header[19]]) {
        62 => Some("x86_64"),
        243 => Some("riscv64"),
        _ => None,
    })
}

pub struct Workload<'a> {
    pub path: &'a Path,
    pub args: &'a [PathBuf],
    pub report: Option<&'a Path>,
    pub timeout: u64,
}

fn stage_app(esp: &Path, app: &Workload) -> io::Result<()> {
    let bytes = fs::read(app.path)?;
    let elf = goblin::elf::Elf::parse(&bytes).map_err(io::Error::other)?;
    if elf.header.e_type != goblin::elf::header::ET_EXEC
        || !matches!(elf.header.e_ident[7], 0 | 3 | 9)
        || elf.interpreter.is_some()
        || !elf.libraries.is_empty()
    {
        return Err(io::Error::other(
            "this port requires a static Linux or FreeBSD ET_EXEC executable",
        ));
    }
    fs::write(esp.join("boot/app"), &bytes)?;
    let mut argv = b"/app/server\0".to_vec();
    if let Some((separator, args)) = app.args.split_first() {
        if separator.as_os_str() != "--" {
            return Err(io::Error::other(
                "this port accepts guest arguments after --",
            ));
        }
        if args.len() >= 256 {
            return Err(io::Error::other("too many guest arguments"));
        }
        for arg in args {
            argv.extend_from_slice(arg.as_os_str().as_encoded_bytes());
            argv.push(0);
        }
    }
    if argv.len() > 32768 {
        return Err(io::Error::other("guest arguments exceed 32 KiB"));
    }
    fs::write(esp.join("boot/argv"), argv)?;
    Ok(())
}

pub fn boot(
    arch: &str,
    memory: Option<&OsStr>,
    qemu: Option<&OsStr>,
    app: Option<Workload>,
) -> io::Result<i32> {
    let (machine, firmware, vars, efi_name, efi, kernel) = match arch {
        "x86_64" => (
            "q35",
            "edk2-x86_64-code.fd",
            "edk2-i386-vars.fd",
            "BOOTX64.EFI",
            spore_kernel::X86_64_EFI,
            spore_kernel::X86_64_ELF,
        ),
        "riscv64" => (
            "virt",
            "edk2-riscv-code.fd",
            "edk2-riscv-vars.fd",
            "BOOTRISCV64.EFI",
            spore_kernel::RISCV64_EFI,
            spore_kernel::RISCV64_ELF,
        ),
        _ => return Err(io::Error::other(format!("unknown architecture: {arch}"))),
    };
    let tmp = Workspace::new()?;
    let esp = tmp.0.join("esp");
    let boot = esp.join("EFI/BOOT");
    fs::create_dir_all(&boot)?;
    fs::create_dir(esp.join("boot"))?;
    fs::write(boot.join(efi_name), efi)?;
    fs::write(boot.join("LICENSE"), spore_kernel::LICENSE)?;
    fs::write(esp.join("boot/kernel.elf"), kernel)?;
    let mut config = include_str!("../../../boot/limine.conf").to_owned();
    if let Some(app) = &app {
        stage_app(&esp, app)?;
        config.push_str("    module_path: boot():/boot/app\n    module_string: /app\n    module_path: boot():/boot/argv\n    module_string: /argv\n");
    }
    fs::write(esp.join("limine.conf"), config)?;
    let qemu = qemu
        .map(OsStr::to_owned)
        .or_else(|| env::var_os("QEMU"))
        .unwrap_or_else(|| format!("qemu-system-{arch}").into());
    let dirs = firmware_dirs(&qemu);
    let lookup =
        |name| find(&dirs, &[name]).ok_or_else(|| io::Error::other(format!("{name} not found")));
    let mut cmd = Command::new(&qemu);
    cmd.args(["-machine", machine, "-smp", "1", "-m"])
        .arg(memory.unwrap_or("256M".as_ref()))
        .args([
            "-display",
            "none",
            "-chardev",
            "stdio,id=console,signal=off",
            "-serial",
            "chardev:console",
            "-monitor",
            "none",
            "-nic",
            "none",
        ])
        .arg("-drive")
        .arg(drive(
            "if=pflash,format=raw,readonly=on,file=",
            lookup(firmware)?.as_os_str(),
            "",
        ))
        .arg("-drive")
        .arg(drive(
            "if=pflash,format=raw,snapshot=on,file=",
            lookup(vars)?.as_os_str(),
            "",
        ))
        .arg("-drive")
        .arg(drive(
            "if=none,id=esp,format=raw,readonly=on,file=fat:",
            esp.as_os_str(),
            "",
        ))
        .args(["-device", "virtio-blk-pci,drive=esp,bootindex=0"]);
    if let Some(app) = app {
        let device = if arch == "x86_64" {
            super::app::StatusDevice::Serial
        } else {
            cmd.args(["-global", "virtio-mmio.force-legacy=false"]);
            super::app::StatusDevice::Virtio
        };
        let q = std::iter::once(cmd.get_program())
            .chain(cmd.get_args())
            .map(OsStr::to_owned)
            .collect();
        return super::app::run(q, app.report, app.timeout, device, b"[kernel] starting app");
    }
    Ok(cmd.arg("-no-reboot").status()?.code().unwrap_or(125))
}
