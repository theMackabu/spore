use std::{
    env,
    path::{Path, PathBuf},
    process::Command,
};

pub fn run(cmd: &mut Command) {
    let status = cmd.status().unwrap_or_else(|e| panic!("{cmd:?}: {e}"));
    assert!(status.success(), "{cmd:?}: {status}");
}

pub fn files(dir: &Path, extensions: &[&str]) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    for entry in dir.read_dir().unwrap() {
        let entry = entry.unwrap();
        let path = entry.path();
        if entry.file_type().unwrap().is_dir() {
            paths.extend(files(&path, extensions));
        } else if path
            .extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| extensions.contains(&e))
        {
            paths.push(path);
        }
    }
    paths.sort();
    paths
}

pub fn kernel_runtime_sources(root: &Path) -> impl Iterator<Item = PathBuf> + '_ {
    [
        "io_uring",
        "mm",
        "fs",
        "proc",
        "net",
        "tty",
        "lib",
        "exec",
        "abi/linux/shared",
    ]
    .into_iter()
    .flat_map(move |dir| {
        let path = root.join(dir);
        watch(&path);
        files(&path, &["c", "S"])
    })
}

pub fn rust_archive(src: &Path, dest: &Path, target: Option<&str>) {
    let mut cmd = Command::new(env::var_os("RUSTC").unwrap());
    cmd.args([
        "--edition=2024",
        "--crate-type=staticlib",
        "-Cpanic=abort",
        "-Copt-level=2",
        "-Ccodegen-units=1",
        "-Clto=fat",
    ])
    .arg(src)
    .arg("-o")
    .arg(dest);
    if let Some(target) = target {
        cmd.args(["--target", target]);
        if target == "aarch64-unknown-none-softfloat" {
            cmd.args(["-Crelocation-model=static", "-Ccode-model=small"]);
        } else if target.ends_with("-none") || target.ends_with("-none-elf") {
            cmd.arg("-Crelocation-model=static");
        } else {
            cmd.arg("-Crelocation-model=pic");
        }
    }
    run(&mut cmd);
}

pub fn output_dir() -> PathBuf {
    PathBuf::from(env::var_os("OUT_DIR").unwrap())
}

pub fn watch(path: &Path) {
    println!("cargo:rerun-if-changed={}", path.display());
}

pub fn build_epoch() -> String {
    println!("cargo:rerun-if-env-changed=SOURCE_DATE_EPOCH");
    env::var("SOURCE_DATE_EPOCH").unwrap_or_else(|_| {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs()
            .to_string()
    })
}
