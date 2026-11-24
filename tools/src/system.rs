use crate::{Workspace, runner::qemu};
use std::{
    env,
    ffi::{OsStr, OsString},
    fs, io,
    os::fd::AsFd,
    os::unix::process::ExitStatusExt,
    path::{Component, Path, PathBuf},
    process::{Command, Stdio},
};

fn status(command: &mut Command) -> io::Result<i32> {
    let result = command.status().map_err(|e| {
        io::Error::new(
            e.kind(),
            format!("{}: {e}", command.get_program().to_string_lossy()),
        )
    })?;
    Ok(result
        .code()
        .unwrap_or_else(|| 128 + result.signal().unwrap_or(0)))
}

pub fn compile(build: &Path, target: &Path) -> io::Result<()> {
    let code = status(
        Command::new(env::var_os("NINJA").unwrap_or_else(|| "ninja".into()))
            .arg("-C")
            .arg(build)
            .arg(target)
            .stdout(Stdio::from(io::stderr().as_fd().try_clone_to_owned()?)),
    )?;
    if code != 0 {
        return Err(io::Error::other(format!(
            "building {} failed ({code})",
            target.display()
        )));
    }
    Ok(())
}

pub fn program(build: &Path, name: &OsStr) -> io::Result<PathBuf> {
    let path = Path::new(name);
    if path.is_file() {
        return Ok(path.into());
    }
    if matches!(path.components().next(), Some(Component::Normal(part)) if part == name)
        && build.join("build.ninja").is_file()
    {
        let target = Path::new("userland/prebuilt").join(name);
        compile(build, &target)?;
        return Ok(build.join(target));
    }
    Err(io::Error::other(format!(
        "executable not found: {}",
        path.display()
    )))
}

fn seed_disk(base: &Path, disk: &Path, reset: bool) -> io::Result<()> {
    if !reset && disk.is_file() {
        return Ok(());
    }
    let tmp = Workspace::at(disk.parent().unwrap_or(Path::new(".")))?;
    let copy = tmp.0.join("root.ext2");
    let mut cp = Command::new("cp");
    if cfg!(target_os = "linux") {
        cp.args(["--reflink=auto", "--sparse=always"]);
    }
    if status(cp.arg(base).arg(&copy))? != 0 {
        return Err(io::Error::other("could not seed persistent root disk"));
    }
    if reset {
        fs::rename(copy, disk)?;
    } else if let Err(e) = fs::hard_link(copy, disk)
        && e.kind() != io::ErrorKind::AlreadyExists
    {
        return Err(e);
    }
    Ok(())
}

pub fn boot(
    build: &Path,
    mut vm: qemu::Config,
    image: Option<OsString>,
    reset: bool,
) -> io::Result<i32> {
    if reset && (vm.snap || vm.root.is_some() || image.is_some()) {
        return Err(io::Error::other(
            "--reset applies to the default persistent root disk",
        ));
    }
    if let Some(image) = image {
        vm.image = image;
    } else {
        let (image, root) = if vm.mode == "integration" {
            ("test_image.img", "test_root.ext2")
        } else {
            ("image.img", "root.ext2")
        };
        compile(build, Path::new(image))?;
        vm.image = build.join(image).into();
        let base = build.join(root);
        if vm.snap {
            vm.root.get_or_insert_with(|| base.into());
        } else {
            let disk = vm
                .root
                .get_or_insert_with(|| build.join("run-root.ext2").into());
            seed_disk(&base, Path::new(disk), reset)?;
        }
    }
    let tmp = Workspace::new()?;
    if vm.vars.is_none() {
        vm.vars = Some(
            if vm.snap || !build.join("build.ninja").is_file() {
                tmp.0.join("vars.fd")
            } else {
                build.join("edk2-vars.fd")
            }
            .into(),
        );
    }
    qemu::run(vm)
}

pub fn check(build: &Path, bench: bool, suite: Option<&OsStr>) -> io::Result<i32> {
    let suite = suite.unwrap_or(OsStr::new(if bench { "guest" } else { "host" }));
    let mode = match (bench, suite.to_str()) {
        (false, Some("host")) => {
            return status(
                Command::new(env::var_os("MESON").unwrap_or_else(|| "meson".into()))
                    .args(["test", "--print-errorlogs", "-C"])
                    .arg(build),
            );
        }
        (_, Some("appliance")) => {
            let target = if bench {
                "measure-appliance"
            } else {
                "check-appliance"
            };
            compile(build, Path::new(target))?;
            return Ok(0);
        }
        (true, Some("inodes")) => {
            let exe = Path::new("tests/bench_ext2_inodes");
            compile(build, exe)?;
            return status(&mut Command::new(build.join(exe)));
        }
        (true, Some("guest")) => "bench",
        (false, Some("guest")) => "integration",
        (false, Some(mode @ ("shell" | "rng"))) => mode,
        _ => {
            return Err(io::Error::other(format!(
                "unknown suite: {}",
                suite.to_string_lossy()
            )));
        }
    };
    let mut vm = crate::vm_config(mode);
    vm.snap = true;
    boot(build, vm, None, false)
}
