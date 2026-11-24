use std::env;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File, OpenOptions, Permissions};
use std::io::{self, BufRead, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{MetadataExt, PermissionsExt, symlink};
use std::path::{Component, Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};

const BLOCK_SIZE: usize = 4096;
const MIB: u64 = 1024 * 1024;

fn run(cmd: &mut Command) -> io::Result<()> {
    let status = cmd.status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "{:?} exited with {status}",
            cmd.get_program()
        )))
    }
}

fn build_image(output: &Path, build: impl FnOnce(&Path) -> io::Result<()>) -> io::Result<()> {
    let parent = output.parent().unwrap_or(Path::new("."));
    let mut suffix = 0u32;
    let temp = loop {
        let path = parent.join(format!(".spore-image-{}-{suffix}", std::process::id()));
        match File::create_new(&path) {
            Ok(_) => break path,
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => suffix += 1,
            Err(e) => return Err(e),
        }
    };
    let result = build(&temp).and_then(|()| fs::rename(&temp, output));
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}

fn create_parents(path: &Path) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    Ok(())
}
fn chmod(path: &Path, mode: u32) -> io::Result<()> {
    fs::set_permissions(path, Permissions::from_mode(mode))
}
fn copy_file(src: &Path, dst: &Path) -> io::Result<()> {
    create_parents(dst)?;
    let mut input =
        File::open(src).map_err(|e| io::Error::new(e.kind(), format!("{}: {e}", src.display())))?;
    io::copy(&mut input, &mut File::create(dst)?)?;
    Ok(())
}
fn remove_if_present(path: &Path) -> io::Result<()> {
    match fs::remove_file(path) {
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
        other => other,
    }
}
fn put_link(target: &Path, dst: &Path) -> io::Result<()> {
    create_parents(dst)?;
    remove_if_present(dst)?;
    symlink(target, dst)
}
fn copy_tree(src: &Path, dst: &Path) -> io::Result<()> {
    let kind = fs::symlink_metadata(src)?.file_type();
    if kind.is_symlink() {
        return put_link(&fs::read_link(src)?, dst);
    }
    if !kind.is_dir() {
        return copy_file(src, dst);
    }
    fs::create_dir_all(dst)?;
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        copy_tree(&entry.path(), &dst.join(entry.file_name()))?;
    }
    Ok(())
}
fn image_path(root: &Path, path: &Path) -> io::Result<PathBuf> {
    if !path.is_absolute() || path.components().any(|p| p == Component::ParentDir) {
        return Err(io::Error::other(format!(
            "invalid image path {}",
            path.display()
        )));
    }
    Ok(root.join(path.strip_prefix("/").unwrap()))
}
fn stage_manifest(root: &Path, manifest: &Path, source: &Path, prebuilt: &Path) -> io::Result<()> {
    let executable = [
        "bin",
        "sbin",
        "usr/bin",
        "usr/sbin",
        "usr/local/bin",
        "usr/libexec",
        "libexec",
        "lib/mycelium",
        "home/spore/demos",
        "etc/update-motd.d",
    ];
    for (i, line) in BufReader::new(File::open(manifest)?)
        .split(b'\n')
        .enumerate()
    {
        let line = line?;
        let parts: Vec<_> = line
            .split(u8::is_ascii_whitespace)
            .filter(|v| !v.is_empty())
            .collect();
        match parts.as_slice() {
            [] => (),
            [comment, ..] if comment.starts_with(b"#") => (),
            [b"dir", dst] => {
                fs::create_dir_all(image_path(root, OsStr::from_bytes(dst).as_ref())?)?
            }
            [b"symlink", target, dst] => put_link(
                OsStr::from_bytes(target).as_ref(),
                &image_path(root, OsStr::from_bytes(dst).as_ref())?,
            )?,
            [src, dst] if *src != b"symlink" => {
                let src = Path::new(OsStr::from_bytes(src));
                let rel = Path::new(OsStr::from_bytes(dst));
                let generated = prebuilt.join(src);
                let src = if generated.exists() {
                    generated
                } else {
                    source.join(src)
                };
                let dst = image_path(root, rel)?;
                if fs::symlink_metadata(&src)?.is_dir() {
                    copy_tree(&src, &dst)?;
                } else {
                    copy_file(&src, &dst)?;
                    let is_exe = executable
                        .iter()
                        .any(|p| rel.strip_prefix("/").unwrap().starts_with(p));
                    chmod(&dst, if is_exe { 0o755 } else { 0o644 })?;
                }
            }
            _ => {
                return Err(io::Error::other(format!(
                    "{}:{}: bad manifest line",
                    manifest.display(),
                    i + 1
                )));
            }
        }
    }
    Ok(())
}
fn gcc_file(name: &str) -> io::Result<PathBuf> {
    let output = Command::new("aarch64-unknown-linux-musl-gcc")
        .arg(format!("-print-file-name={name}"))
        .output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "musl GCC could not locate {name}"
        )));
    }
    Ok(PathBuf::from(OsString::from_vec(
        output.stdout.trim_ascii_end().to_vec(),
    )))
}
fn install_musl(root: &Path) -> io::Result<()> {
    let libc = gcc_file("libc.so")?;
    let ld = gcc_file("ld-musl-aarch64.so.1")?;
    let dest = root.join("lib/libc.so");
    copy_file(&libc, &dest)?;
    chmod(&dest, 0o755)?;
    let a = fs::metadata(&ld)?;
    let b = fs::metadata(&libc)?;
    let dest = root.join("lib/ld-musl-aarch64.so.1");
    if (a.dev(), a.ino()) == (b.dev(), b.ino()) {
        put_link(Path::new("libc.so"), &dest)?;
    } else {
        copy_file(&ld, &dest)?;
        chmod(&dest, 0o755)?;
    }
    let headers = gcc_file("include")?
        .join("../../../../../aarch64-unknown-linux-musl/sys-include")
        .canonicalize()?;
    copy_tree(&headers, &root.join("usr/include"))?;
    for obj in ["crt1.o", "crti.o", "crtn.o", "Scrt1.o", "rcrt1.o"] {
        let src = gcc_file(obj)?;
        if src.exists() {
            let dst = root.join("usr/lib").join(obj);
            copy_file(&src, &dst)?;
            chmod(&dst, 0o644)?;
        }
    }
    put_link(Path::new("/lib/libc.so"), &root.join("usr/lib/libc.so"))
}
fn accounts(root: &Path) -> io::Result<()> {
    for dir in ["root/.ssh", "home/spore/.ssh"] {
        let path = root.join(dir);
        fs::create_dir_all(&path)?;
        chmod(&path, 0o700)?;
    }
    for (path, mode, contents) in [
        (
            "etc/passwd",
            0o644,
            "root:x:0:0:root:/root:/bin/msh\nspore:x:1000:1000:spore:/home/spore:/bin/msh\n",
        ),
        (
            "etc/group",
            0o644,
            "root:x:0:\nspore:x:1000:\nsudo:x:27:spore\n",
        ),
        (
            "etc/shadow",
            0o600,
            "root:!:0:0:99999:7:::\nspore::0:0:99999:7:::\n",
        ),
        (
            "etc/sudoers",
            0o440,
            "Defaults !use_pty\nroot ALL=(ALL) ALL\nspore ALL=(ALL) NOPASSWD: ALL\n",
        ),
        ("root/.profile", 0o644, "# root login profile for msh\n"),
        ("root/.mshrc", 0o644, "# interactive root msh startup\n"),
        (
            "home/spore/.profile",
            0o644,
            "# spore login profile for msh\n",
        ),
        (
            "home/spore/.mshrc",
            0o644,
            "# interactive spore msh startup\n",
        ),
    ] {
        let path = root.join(path);
        fs::write(&path, contents)?;
        chmod(&path, mode)?;
    }
    Ok(())
}
fn write_owners(script: &mut impl Write, root: &Path, rel: &Path) -> io::Result<()> {
    let mut name = Vec::from(b"/".as_slice());
    name.extend_from_slice(rel.as_os_str().as_bytes());
    if name.iter().any(u8::is_ascii_whitespace) {
        return Err(io::Error::other("debugfs path contains whitespace"));
    }
    let owner = if rel.starts_with("home/spore") {
        1000
    } else {
        0
    };
    for field in ["uid", "gid"] {
        script.write_all(b"set_inode_field ")?;
        script.write_all(&name)?;
        writeln!(script, " {field} {owner}")?;
    }
    let path = root.join(rel);
    if fs::symlink_metadata(&path)?.is_dir() {
        for entry in fs::read_dir(path)? {
            write_owners(script, root, &rel.join(entry?.file_name()))?;
        }
    }
    Ok(())
}
fn build_root(
    source: &Path,
    stage: &Path,
    manifest: &Path,
    prebuilt: &Path,
    image: &Path,
) -> io::Result<()> {
    match fs::remove_dir_all(stage) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => return Err(e),
        _ => (),
    }
    let root = stage.join("rootfs");
    fs::create_dir_all(&root)?;
    stage_manifest(&root, manifest, source, prebuilt)?;
    for dir in ["dev", "etc", "proc", "sys", "tmp"] {
        fs::create_dir_all(root.join(dir))?;
    }
    install_musl(&root)?;
    accounts(&root)?;
    let seed = root.join(".spore-inodes");
    let mut block = [0u8; BLOCK_SIZE];
    let fields = [
        0x4e495053u32,
        1,
        64,
        256,
        0x40000000,
        0,
        0,
        u32::MAX,
        0,
        256,
    ];
    for (word, value) in block.chunks_exact_mut(4).zip(fields) {
        word.copy_from_slice(&value.to_le_bytes());
    }
    fs::write(&seed, block)?;
    chmod(&seed, 0o600)?;
    run(Command::new("mke2fs")
        .args([
            "-q",
            "-t",
            "ext2",
            "-b",
            "4096",
            "-N",
            "8192",
            "-O",
            "^resize_inode",
            "-E",
            "assume_storage_prezeroed=1",
            "-d",
        ])
        .arg(&root)
        .arg(image)
        .arg((10 * 1024 * MIB / BLOCK_SIZE as u64).to_string()))?;
    let commands = stage.join("ownership.debugfs");
    {
        let mut script = BufWriter::new(File::create(&commands)?);
        write_owners(&mut script, &root, Path::new(""))?;
        for bin in ["sudo", "su", "passwd"] {
            if root.join("bin").join(bin).exists() {
                writeln!(script, "set_inode_field /bin/{bin} mode 0104755")?;
            }
        }
        script.flush()?;
    }
    let rc = run(Command::new("debugfs")
        .args(["-w", "-f"])
        .arg(&commands)
        .arg(image)
        .stdout(Stdio::null())
        .stderr(Stdio::null()));
    fs::remove_file(commands)?;
    rc?;
    let mut disk = OpenOptions::new().read(true).write(true).open(image)?;
    let incompat = SeekFrom::Start(1024 + 96);
    disk.seek(incompat)?;
    let mut flags = [0; 4];
    disk.read_exact(&mut flags)?;
    disk.seek(incompat)?;
    disk.write_all(&(u32::from_le_bytes(flags) | 0x8000_0000).to_le_bytes())
}
fn build_esp(
    kernel: &Path,
    boot: &Path,
    config: &Path,
    license: &Path,
    image: &Path,
) -> io::Result<()> {
    let payload = fs::metadata(kernel)?.len()
        + fs::metadata(boot)?.len()
        + fs::metadata(config)?.len()
        + fs::metadata(license)?.len();
    let mib = (payload * 2 + 2 * MIB).div_ceil(MIB).max(9);
    File::create(image)?.set_len(mib * MIB)?;
    run(Command::new("mkfs.fat")
        .args(["-F", "16"])
        .arg(image)
        .stdout(Stdio::null())
        .stderr(Stdio::null()))?;
    run(Command::new("mmd")
        .arg("-i")
        .arg(image)
        .args(["::/EFI", "::/EFI/BOOT", "::/boot"]))?;
    for (src, dst) in [
        (boot, "::/EFI/BOOT/BOOTAA64.EFI"),
        (kernel, "::/boot/kernel.elf"),
        (config, "::/limine.conf"),
        (license, "::/EFI/BOOT/LICENSE"),
    ] {
        run(Command::new("mcopy").arg("-i").arg(image).arg(src).arg(dst))?;
    }
    Ok(())
}
fn main() -> ExitCode {
    let args: Vec<PathBuf> = env::args_os().skip(1).map(PathBuf::from).collect();
    let rc = match args.as_slice() {
        [mode, k, b, c, l, o] if mode == Path::new("esp") => {
            build_image(o, |tmp| build_esp(k, b, c, l, tmp))
        }
        [mode, s, t, m, p, o] if mode == Path::new("rootfs") => {
            build_image(o, |tmp| build_root(s, t, m, p, tmp))
        }
        _ => {
            eprintln!(
                "usage: spore-image esp KERNEL_ELF BOOT_EFI CONFIG LICENSE OUTPUT\n       spore-image rootfs SOURCE_ROOT STAGE MANIFEST PREBUILT_ROOT OUTPUT"
            );
            return ExitCode::from(2);
        }
    };
    match rc {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("spore-image: {e}");
            ExitCode::FAILURE
        }
    }
}
