use crate::{Workspace, runner::qemu};
use std::{
    env,
    ffi::{OsStr, OsString},
    fs, io,
    os::unix::process::ExitStatusExt,
    path::{Component, Path, PathBuf},
    process::Command,
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

fn cargo(action: &str, build: &Path) -> Command {
    let mut cmd = Command::new("cargo");
    cmd.current_dir(env!("CARGO_MANIFEST_DIR"))
        .args([action, "--locked", "--target-dir"])
        .arg(build.parent().unwrap());
    if build.ends_with("release") {
        cmd.arg("--release");
    }
    cmd
}

pub fn build_project(build: &Path, full: bool, tests: bool) -> io::Result<i32> {
    if tests && !full {
        userland()?;
    }
    if full || tests {
        let mut cmd = cargo("run", build);
        cmd.args(["--features", "userland", "--", "_image"]);
        if tests {
            cmd.arg("tests");
        }
        return status(&mut cmd);
    }
    let mut cmd = cargo("build", build);
    if cfg!(feature = "userland") {
        cmd.args(["--features", "userland"]);
    }
    status(&mut cmd)
}

pub fn userland() -> io::Result<&'static Path> {
    #[cfg(feature = "userland")]
    {
        Ok(Path::new(spore_userland::ARTIFACTS))
    }
    #[cfg(not(feature = "userland"))]
    {
        Err(io::Error::other(
            "enable full userland with spore build --userland",
        ))
    }
}

pub fn program(name: &OsStr) -> io::Result<PathBuf> {
    let path = Path::new(name);
    if path.is_file() {
        return Ok(path.into());
    }
    if matches!(path.components().next(), Some(Component::Normal(part)) if part == name) {
        for package in spore_packages::catalog().packages {
            if let Some(member) = package.files.iter().find(|p| p.file_name() == Some(name)) {
                let cache = env::var_os("XDG_CACHE_HOME")
                    .map(PathBuf::from)
                    .or_else(|| env::var_os("HOME").map(|p| PathBuf::from(p).join(".cache")))
                    .ok_or_else(|| io::Error::other("package cache needs HOME or XDG_CACHE_HOME"))?
                    .join("spore");
                let dest = cache.join("bin").join(&package.archive.sha256);
                let exe = dest.join(name);
                if !exe.is_file() {
                    let local = Path::new(env!("CARGO_MANIFEST_DIR")).join(".cache/packages");
                    let archives = if local.join(&package.archive.sha256).is_file() {
                        local
                    } else {
                        cache.join("packages")
                    };
                    package
                        .archive
                        .extract(&archives, &dest, std::slice::from_ref(member))?;
                }
                return Ok(exe);
            }
        }
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
    let image_supplied = image.is_some();
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
        if !build.join(image).is_file() || (vm.root.is_none() && !build.join(root).is_file()) {
            crate::image::system_image(build, vm.mode == "integration")?;
        }
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
    if vm.vars.is_none() && !vm.snap && !image_supplied {
        vm.vars = Some(build.join("edk2-vars.fd").into());
    }
    qemu::run(vm)
}

pub fn check(build: &Path, bench: bool, suite: Option<&OsStr>) -> io::Result<i32> {
    let suite = suite.unwrap_or(OsStr::new(if bench { "guest" } else { "host" }));
    let mode = match (bench, suite.to_str()) {
        (false, Some("host")) | (true, Some("inodes")) => {
            let mut cmd = cargo("test", build);
            cmd.args(["--test", "host"]);
            if cfg!(feature = "userland") {
                cmd.args(["--features", "userland"]);
            }
            if bench {
                cmd.args(["bench_inodes", "--", "--ignored", "--nocapture"]);
            }
            if build.join("root.ext2").is_file() {
                cmd.env("SPORE_TEST_ROOT", build.join("root.ext2"));
            }
            return status(&mut cmd);
        }
        (false, Some("ports")) => {
            let mut cmd = Command::new("python3");
            cmd.arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/ports/check.py"))
                .arg(build.join("spore"));
            if cfg!(feature = "userland") {
                cmd.arg("--userland");
            }
            return status(&mut cmd);
        }
        (false, Some("stress")) | (true, Some("startup")) | (_, Some("appliance")) => {
            let script = match suite.to_str() {
                Some("stress") => "stress.py",
                Some("startup") => "startup.py",
                _ => "check.py",
            };
            let mut cmd = Command::new("python3");
            cmd.arg(
                Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("tests/appliance")
                    .join(script),
            )
            .env("BUILD_DIR", build);
            if bench && suite == "appliance" {
                cmd.arg("--sweep");
            }
            return status(&mut cmd);
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
    userland()?;
    let code = build_project(build, true, mode == "integration")?;
    if code != 0 {
        return Ok(code);
    }
    let mut vm = crate::vm_config(mode);
    vm.snap = true;
    boot(build, vm, None, false)
}
