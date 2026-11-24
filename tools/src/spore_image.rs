use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::unix::fs::{symlink, MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const EXT2_FEATURE_INCOMPAT_OFFSET: u64 = 1120;
const EXT2_INCOMPAT_SPORE_EXT2PLUS: u32 = 0x8000_0000;
const ROOT_BLOCK_SIZE: u64 = 4096;
const ROOT_STATIC_INODES: u64 = 8192;
const ROOT_BLOCKS: u64 = 10 * 1024 * 1024 * 1024 / ROOT_BLOCK_SIZE;

fn die<T>(r: io::Result<T>, path: &str) -> T {
    r.unwrap_or_else(|e| panic!("{path}: {e}"))
}
fn text(path: &Path, data: &str, mode: u32) {
    if let Some(parent) = path.parent() {
        die(fs::create_dir_all(parent), "mkdir");
    }
    die(fs::write(path, data), &path.display().to_string());
    die(
        fs::set_permissions(path, fs::Permissions::from_mode(mode)),
        "chmod",
    );
}
fn run(args: &[String], quiet: bool) {
    let mut c = Command::new(&args[0]);
    c.args(&args[1..]);
    if quiet {
        c.stdout(Stdio::null()).stderr(Stdio::null());
    }
    let status = die(c.status(), &args[0]);
    if !status.success() {
        panic!("command failed");
    }
}
fn command_output(args: &[&str]) -> String {
    let out = die(Command::new(args[0]).args(&args[1..]).output(), args[0]);
    if !out.status.success() {
        panic!("command failed");
    }
    String::from_utf8(out.stdout).unwrap().trim().to_owned()
}
fn copy_file(src: &Path, dst: &Path) {
    if let Some(p) = dst.parent() {
        die(fs::create_dir_all(p), "mkdir");
    }
    die(fs::copy(src, dst), "copy");
}
fn copy_tree(src: &Path, dst: &Path) {
    let meta = die(fs::symlink_metadata(src), "lstat");
    if meta.file_type().is_symlink() {
        let _ = fs::remove_file(dst);
        die(symlink(die(fs::read_link(src), "readlink"), dst), "symlink");
        return;
    }
    if !meta.is_dir() {
        copy_file(src, dst);
        return;
    }
    die(fs::create_dir_all(dst), "mkdir");
    for e in die(fs::read_dir(src), "opendir") {
        let e = die(e, "readdir");
        copy_tree(&e.path(), &dst.join(e.file_name()));
    }
}
fn is_dir(path: &Path) -> bool {
    die(fs::symlink_metadata(path), "lstat").is_dir()
}
fn executable(dst: &str) -> bool {
    [
        "/bin/",
        "/sbin/",
        "/usr/bin/",
        "/usr/sbin/",
        "/usr/local/bin/",
        "/usr/libexec/",
        "/libexec/",
        "/lib/mycelium/",
        "/home/spore/demos/",
        "/etc/update-motd.d/",
    ]
    .iter()
    .any(|p| dst.starts_with(p))
}
fn root_path(root: &Path, target: &str) -> PathBuf {
    root.join(target.strip_prefix('/').unwrap_or(target))
}
fn copy_into(src: &Path, root: &Path, target: &str) {
    let out = root_path(root, target);
    if is_dir(src) {
        copy_tree(src, &out);
    } else {
        copy_file(src, &out);
        die(
            fs::set_permissions(
                out,
                fs::Permissions::from_mode(if executable(target) { 0o755 } else { 0o644 }),
            ),
            "chmod",
        );
    }
}
fn link_into(target: &str, root: &Path, link: &str) {
    let out = root_path(root, link);
    if let Some(p) = out.parent() {
        die(fs::create_dir_all(p), "mkdir")
    };
    let _ = fs::remove_file(&out);
    die(symlink(target, &out), "symlink");
}
fn debugfs(image: &Path, script: &Path) {
    run(
        &[
            "debugfs".into(),
            "-w".into(),
            "-f".into(),
            script.display().to_string(),
            image.display().to_string(),
        ],
        true,
    );
}
fn write_debugfs(image: &Path, script: &str) {
    let p = PathBuf::from(format!("{}.debugfs", image.display()));
    text(&p, script, 0o644);
    debugfs(image, &p);
    let _ = fs::remove_file(p);
}
fn chown_commands(out: &mut String, host: &Path, image: &str, uid: u32, gid: u32) {
    if image
        .bytes()
        .any(|c| matches!(c, b' ' | b'\n' | b'\r' | b'\t'))
    {
        panic!("debugfs path contains whitespace");
    }
    out.push_str(&format!(
        "set_inode_field {image} uid {uid}\nset_inode_field {image} gid {gid}\n"
    ));
    if !die(fs::symlink_metadata(host), "lstat").is_dir() {
        return;
    }
    for e in die(fs::read_dir(host), "opendir") {
        let e = die(e, "readdir");
        let n = e.file_name().into_string().unwrap();
        let child = if image == "/" {
            format!("/{n}")
        } else {
            format!("{image}/{n}")
        };
        chown_commands(out, &e.path(), &child, uid, gid);
    }
}
fn chown_tree(image: &Path, root: &Path, image_path: &str, uid: u32, gid: u32) {
    let host = root_path(root, image_path);
    if !host.exists() {
        return;
    };
    let mut s = String::new();
    chown_commands(&mut s, &host, image_path, uid, gid);
    let p = PathBuf::from(format!("{}.chown.debugfs", image.display()));
    text(&p, &s, 0o644);
    debugfs(image, &p);
    let _ = fs::remove_file(p);
}
fn set_ext2plus(image: &Path) {
    let mut f = die(
        OpenOptions::new().read(true).write(true).open(image),
        "open",
    );
    die(
        f.seek(SeekFrom::Start(EXT2_FEATURE_INCOMPAT_OFFSET)),
        "seek",
    );
    let mut b = [0; 4];
    die(f.read_exact(&mut b), "read");
    let n = u32::from_le_bytes(b) | EXT2_INCOMPAT_SPORE_EXT2PLUS;
    die(
        f.seek(SeekFrom::Start(EXT2_FEATURE_INCOMPAT_OFFSET)),
        "seek",
    );
    die(f.write_all(&n.to_le_bytes()), "write");
}
fn musl(root: &Path) {
    let lib = root.join("lib");
    die(fs::create_dir_all(&lib), "mkdir");
    let ld = command_output(&[
        "aarch64-unknown-linux-musl-gcc",
        "-print-file-name=ld-musl-aarch64.so.1",
    ]);
    let libc = command_output(&["aarch64-unknown-linux-musl-gcc", "-print-file-name=libc.so"]);
    copy_file(Path::new(&libc), &lib.join("libc.so"));
    die(
        fs::set_permissions(lib.join("libc.so"), fs::Permissions::from_mode(0o755)),
        "chmod",
    );
    let a = die(fs::metadata(&ld), "stat");
    let b = die(fs::metadata(&libc), "stat");
    if a.dev() == b.dev() && a.ino() == b.ino() {
        link_into("libc.so", root, "/lib/ld-musl-aarch64.so.1")
    } else {
        copy_file(Path::new(&ld), &lib.join("ld-musl-aarch64.so.1"));
        die(
            fs::set_permissions(
                lib.join("ld-musl-aarch64.so.1"),
                fs::Permissions::from_mode(0o755),
            ),
            "chmod",
        )
    }
}
fn musl_devel(root: &Path) {
    let inc = root.join("usr/include");
    let lib = root.join("usr/lib");
    die(fs::create_dir_all(&inc), "mkdir");
    die(fs::create_dir_all(&lib), "mkdir");
    let gcc = command_output(&["aarch64-unknown-linux-musl-gcc", "-print-file-name=include"]);
    let src = die(
        fs::canonicalize(
            Path::new(&gcc).join("../../../../../aarch64-unknown-linux-musl/sys-include"),
        ),
        "realpath musl include",
    );
    copy_tree(&src, &inc);
    for n in ["crt1.o", "crti.o", "crtn.o", "Scrt1.o", "rcrt1.o"] {
        let s = command_output(&[
            "aarch64-unknown-linux-musl-gcc",
            &format!("-print-file-name={n}"),
        ]);
        if Path::new(&s).exists() {
            copy_file(Path::new(&s), &lib.join(n));
            die(
                fs::set_permissions(lib.join(n), fs::Permissions::from_mode(0o644)),
                "chmod",
            )
        }
    }
    link_into("/lib/libc.so", root, "/usr/lib/libc.so");
}
fn accounts(root: &Path) {
    for d in ["root", "home/spore", "root/.ssh", "home/spore/.ssh"] {
        die(fs::create_dir_all(root.join(d)), "mkdir");
    }
    die(
        fs::set_permissions(root.join("root/.ssh"), fs::Permissions::from_mode(0o700)),
        "chmod",
    );
    die(
        fs::set_permissions(
            root.join("home/spore/.ssh"),
            fs::Permissions::from_mode(0o700),
        ),
        "chmod",
    );
    text(
        &root_path(root, "/etc/passwd"),
        "root:x:0:0:root:/root:/bin/msh\nspore:x:1000:1000:spore:/home/spore:/bin/msh\n",
        0o644,
    );
    text(
        &root_path(root, "/etc/group"),
        "root:x:0:\nspore:x:1000:\nsudo:x:27:spore\n",
        0o644,
    );
    text(
        &root_path(root, "/etc/shadow"),
        "root:!:0:0:99999:7:::\nspore::0:0:99999:7:::\n",
        0o600,
    );
    text(
        &root_path(root, "/etc/sudoers"),
        "Defaults !use_pty\nroot ALL=(ALL) ALL\nspore ALL=(ALL) NOPASSWD: ALL\n",
        0o440,
    );
    text(
        &root_path(root, "/root/.profile"),
        "# root login profile for msh\n",
        0o644,
    );
    text(
        &root_path(root, "/root/.mshrc"),
        "# interactive root msh startup\n",
        0o644,
    );
    text(
        &root_path(root, "/home/spore/.profile"),
        "# spore login profile for msh\n",
        0o644,
    );
    text(
        &root_path(root, "/home/spore/.mshrc"),
        "# interactive spore msh startup\n",
        0o644,
    );
}
fn build_rootfs(source: &Path, stage: &Path, manifest: &Path, prebuilt: &Path, out: &Path) {
    let _ = fs::remove_dir_all(stage);
    let root = stage.join("rootfs");
    die(fs::create_dir_all(&root), "mkdir");
    for line in die(fs::read_to_string(manifest), "manifest").lines() {
        let v: Vec<_> = line.split_whitespace().collect();
        if v.is_empty() || v[0].starts_with('#') {
            continue;
        }
        if v[0] == "dir" {
            die(fs::create_dir_all(root_path(&root, v[1])), "mkdir");
            continue;
        }
        if v[0] == "symlink" {
            link_into(v[1], &root, v[2]);
            continue;
        }
        let p = prebuilt.join(v[0]);
        let fallback = source.join(v[0]);
        copy_into(if p.exists() { &p } else { &fallback }, &root, v[1]);
    }
    for d in ["dev", "etc", "proc", "sys", "tmp"] {
        die(fs::create_dir_all(root.join(d)), "mkdir");
    }
    musl(&root);
    musl_devel(&root);
    accounts(&root);
    let seed = root.join(".spore-inodes");
    let mut b = vec![0u8; 4096];
    for (i, v) in [
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
        0,
    ]
    .iter()
    .enumerate()
    {
        b[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes())
    }
    die(fs::write(&seed, b), "seed");
    die(
        fs::set_permissions(&seed, fs::Permissions::from_mode(0o600)),
        "chmod",
    );
    let _ = fs::remove_file(out);
    run(
        &[
            "mke2fs".into(),
            "-q".into(),
            "-t".into(),
            "ext2".into(),
            "-b".into(),
            "4096".into(),
            "-N".into(),
            ROOT_STATIC_INODES.to_string(),
            "-O".into(),
            "^resize_inode".into(),
            "-E".into(),
            "assume_storage_prezeroed=1".into(),
            "-d".into(),
            root.display().to_string(),
            out.display().to_string(),
            ROOT_BLOCKS.to_string(),
        ],
        false,
    );
    chown_tree(out, &root, "/", 0, 0);
    chown_tree(out, &root, "/home/spore", 1000, 1000);
    for n in ["sudo", "su", "passwd"] {
        if root.join("bin").join(n).exists() {
            write_debugfs(out,&format!("set_inode_field /bin/{n} uid 0\nset_inode_field /bin/{n} gid 0\nset_inode_field /bin/{n} mode 0104755\n"))
        }
    }
    write_debugfs(out,"set_inode_field /etc/shadow uid 0\nset_inode_field /etc/shadow gid 0\nset_inode_field /etc/sudoers uid 0\nset_inode_field /etc/sudoers gid 0\n");
    set_ext2plus(out);
}
fn esp(kernel: &Path, boot: &Path, config: &Path, license: &Path, out: &Path) {
    let payload = die(fs::metadata(boot), "stat").len()
        + die(fs::metadata(kernel), "stat").len()
        + die(fs::metadata(config), "stat").len()
        + die(fs::metadata(license), "stat").len();
    let size =
        ((payload * 2 + 2 * 1024 * 1024 + 1024 * 1024 - 1) / (1024 * 1024)).max(9) * 1024 * 1024;
    let f = die(File::create(out), "create");
    die(f.set_len(size), "truncate");
    run(
        &[
            "mkfs.fat".into(),
            "-F".into(),
            "16".into(),
            out.display().to_string(),
        ],
        true,
    );
    run(
        &[
            "mmd".into(),
            "-i".into(),
            out.display().to_string(),
            "::/EFI".into(),
            "::/EFI/BOOT".into(),
            "::/boot".into(),
        ],
        false,
    );
    for (s, d) in [
        (boot, "/EFI/BOOT/BOOTAA64.EFI"),
        (kernel, "/boot/kernel.elf"),
        (config, "/limine.conf"),
        (license, "/EFI/BOOT/LICENSE"),
    ] {
        run(
            &[
                "mcopy".into(),
                "-i".into(),
                out.display().to_string(),
                s.display().to_string(),
                format!("::{d}"),
            ],
            false,
        )
    }
}
fn main() {
    let a: Vec<String> = env::args().collect();
    if a.len() == 7 && a[1] == "esp" {
        esp(
            Path::new(&a[2]),
            Path::new(&a[3]),
            Path::new(&a[4]),
            Path::new(&a[5]),
            Path::new(&a[6]),
        );
        return;
    }
    if a.len() == 7 && a[1] == "rootfs" {
        build_rootfs(
            Path::new(&a[2]),
            Path::new(&a[3]),
            Path::new(&a[4]),
            Path::new(&a[5]),
            Path::new(&a[6]),
        );
        return;
    }
    eprintln!("usage: spore-image esp KERNEL_ELF BOOT_EFI CONFIG LICENSE OUTPUT_IMAGE\n       spore-image rootfs SOURCE_ROOT STAGE_DIR MANIFEST PREBUILT_ROOT OUTPUT_ROOT");
    std::process::exit(2)
}
