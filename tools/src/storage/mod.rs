mod merge;
mod packages;
mod volume;

use crate::Workspace;
use std::{
    fs::{self, File, OpenOptions},
    io,
    os::unix::fs::{FileExt, MetadataExt, PermissionsExt, symlink},
    path::{Path, PathBuf},
    process::Command,
};

pub struct Disks {
    pub image: PathBuf,
    pub root: PathBuf,
    pub qcow: bool,
}

fn lock(build: &Path, name: &str) -> io::Result<File> {
    let dir = build.join("system");
    fs::create_dir_all(&dir)?;
    let file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(dir.join(name))?;
    file.try_lock()
        .map_err(|e| io::Error::other(format!("system disk busy ({name}): {e}")))?;
    Ok(file)
}

pub fn session(build: &Path) -> io::Result<File> {
    lock(build, "session.lock")
}

fn uuid(path: &Path) -> io::Result<[u8; 16]> {
    let mut id = [0; 16];
    File::open(path)?.read_exact_at(&mut id, 1024 + 104)?;
    Ok(id)
}

fn link_once(src: &Path, dst: &Path) -> io::Result<()> {
    match fs::hard_link(src, dst) {
        Err(e) if e.kind() == io::ErrorKind::AlreadyExists => Ok(()),
        result => result,
    }
}

pub fn publish(build: &Path, stage: &Path) -> io::Result<()> {
    let _lock = lock(build, "base.lock")?;
    let state = build.join("system");
    let legacy = build.join("run-root.ext2");
    let base = build.join("root.ext2");
    if !state.join("current").try_exists()?
        && legacy.try_exists()?
        && build.join("image.img").try_exists()?
    {
        link_once(&build.join("image.img"), &state.join("original.img"))?;
        if base.try_exists()? && uuid(&legacy)? == uuid(&base)? {
            link_once(&base, &state.join("original-base.ext2"))?;
        }
    }
    let previous = match fs::read_link(state.join("latest")) {
        Ok(target) => Some(target),
        Err(e) if e.kind() == io::ErrorKind::NotFound => None,
        Err(e) => return Err(e),
    };
    let pair = Workspace::at(&state)?;
    for file in ["root.ext2", "image.img"] {
        crate::system::copy_disk(&stage.join(file), &pair.0.join(file))?;
        fs::set_permissions(pair.0.join(file), fs::Permissions::from_mode(0o444))?;
        File::open(pair.0.join(file))?.sync_all()?;
    }
    let id: String = uuid(&pair.0.join("root.ext2"))?
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let dir = format!("base-{id}");
    fs::rename(&pair.0, state.join(&dir))?;
    select(&state, "latest", &dir)?;
    for file in ["root.ext2", "image.img"] {
        fs::rename(stage.join(file), build.join(file))?;
    }
    if let Some(previous) = previous
        && previous.components().count() == 1
        && previous.to_str().is_some_and(|s| {
            s.strip_prefix("base-")
                .is_some_and(|id| id.len() == 32 && id.bytes().all(|b| b.is_ascii_hexdigit()))
        })
    {
        let old = state.join(previous);
        for name in ["root.ext2", "image.img"] {
            fs::remove_file(old.join(name))?;
        }
        fs::remove_dir(old)?;
    }
    Ok(())
}

fn select(state: &Path, name: &str, target: &str) -> io::Result<()> {
    let tmp = Workspace::at(state)?;
    let link = tmp.0.join("link");
    symlink(target, &link)?;
    fs::rename(link, state.join(name))?;
    File::open(state)?.sync_all()
}

fn qemu_img(cmd: &mut Command) -> io::Result<()> {
    let rc = cmd
        .status()
        .map_err(|e| io::Error::new(e.kind(), format!("qemu-img: {e}")))?;
    if rc.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!("qemu-img exited with {rc}")))
    }
}

fn has_selection(current: &Path) -> io::Result<bool> {
    match fs::symlink_metadata(current) {
        Ok(_) => {
            for name in ["base.ext2", "image.img", "data.qcow2"] {
                File::open(current.join(name)).map_err(|e| {
                    io::Error::other(format!("selected installation is missing {name}: {e}"))
                })?;
            }
            Ok(true)
        }
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(false),
        Err(e) => Err(e),
    }
}

fn same_base(a: &Path, b: &Path) -> io::Result<bool> {
    let left = fs::metadata(a)?;
    let right = fs::metadata(b)?;
    if (left.dev(), left.ino()) == (right.dev(), right.ino()) {
        return Ok(true);
    }
    if uuid(a)? != uuid(b)? {
        return Ok(false);
    }
    match Command::new("qemu-img")
        .args(["compare", "-q", "-f", "raw", "-F", "raw"])
        .arg(a)
        .arg(b)
        .status()?
        .code()
    {
        Some(0) => Ok(true),
        Some(1) => Ok(false),
        _ => Err(io::Error::other("cannot compare base disks")),
    }
}

fn selected(dir: &Path) -> Disks {
    Disks {
        image: dir.join("image.img"),
        root: dir.join("data.qcow2"),
        qcow: true,
    }
}

pub fn boot(build: &Path, reset: bool) -> io::Result<Disks> {
    let state = build.join("system");
    let current = state.join("current");
    if !reset {
        if has_selection(&current)? {
            return Ok(selected(&current));
        }
        let legacy = build.join("run-root.ext2");
        if legacy.try_exists()? {
            let _lock = lock(build, "base.lock")?;
            let original = state.join("original.img");
            return Ok(Disks {
                image: if original.try_exists()? {
                    original
                } else {
                    build.join("image.img")
                },
                root: legacy,
                qcow: false,
            });
        }
    }
    install(build, true, None)?;
    Ok(selected(&current))
}

pub fn upgrade(build: &Path, from: Option<&Path>) -> io::Result<()> {
    let _session = session(build)?;
    install(build, false, from)
}

fn install(build: &Path, fresh: bool, from: Option<&Path>) -> io::Result<()> {
    let state = build.join("system");
    let current = state.join("current");
    let stage = Workspace::at(&state)?;
    let base = stage.0.join("base.ext2");
    {
        let _lock = lock(build, "base.lock")?;
        let latest = state.join("latest");
        let published = latest.try_exists()?;
        let artifacts = if published { &latest } else { build };
        for (src, dst) in [("root.ext2", "base.ext2"), ("image.img", "image.img")] {
            let src_path = artifacts.join(src);
            let dst_path = stage.0.join(dst);
            let pinned = if published {
                fs::hard_link(src_path, dst_path)
            } else {
                crate::system::copy_disk(&src_path, &dst_path)
                    .and_then(|()| fs::set_permissions(dst_path, fs::Permissions::from_mode(0o444)))
            };
            pinned.map_err(|e| {
                io::Error::other(format!(
                    "cannot pin {src} ({e}); build with spore build --userland"
                ))
            })?;
        }
    }

    let active = has_selection(&current)?;
    if from.is_some() && (active || !build.join("run-root.ext2").is_file()) {
        return Err(io::Error::other(
            "--from-base requires an existing raw installation",
        ));
    }
    if active && !fresh {
        if same_base(&base, &current.join("base.ext2"))?
            && fs::read(stage.0.join("image.img"))? == fs::read(current.join("image.img"))?
        {
            println!("already running the selected build");
            return Ok(());
        }
    }

    let source = if active {
        current.join("data.qcow2")
    } else {
        build.join("run-root.ext2")
    };
    let updating = !fresh && source.try_exists()?;
    if let Some(p) = from {
        let old = fs::metadata(p)?;
        let live = fs::metadata(&source)?;
        if (old.dev(), old.ino()) == (live.dev(), live.ino()) {
            return Err(io::Error::other(
                "--from-base must name the original pristine base",
            ));
        }
    }
    let merged = stage.0.join("merged.raw");
    if updating {
        let old = if active {
            Some(current.join("base.ext2"))
        } else if let Some(p) = from {
            Some(p.into())
        } else if state.join("original-base.ext2").try_exists()? {
            Some(state.join("original-base.ext2"))
        } else if uuid(&source)? == uuid(&base)? {
            Some(base.clone())
        } else {
            None
        };
        if !active
            && let Some(p) = &old
            && uuid(p)? != uuid(&source)?
        {
            return Err(io::Error::other(
                "previous base UUID differs from the installed disk",
            ));
        }
        if old.is_none() {
            eprintln!(
                "original base unavailable; adding missing defaults and keeping differing local files with .spore-new copies"
            );
        }
        let format = if active { "qcow2" } else { "raw" };
        qemu_img(
            Command::new("qemu-img")
                .args(["convert", "-f", format, "-O", "raw"])
                .arg(&source)
                .arg(&merged),
        )?;
        if fs::metadata(&merged)?.len() != fs::metadata(&base)?.len() {
            return Err(io::Error::other(
                "upgrade requires matching virtual disk capacities",
            ));
        }
        merge::apply(old.as_deref(), &base, &merged)?;
    }
    let overlay = "data.qcow2";
    let backing = if updating { "merged.raw" } else { "base.ext2" };
    qemu_img(Command::new("qemu-img").current_dir(&stage.0).args([
        "create", "-q", "-f", "qcow2", "-F", "raw", "-b", backing, overlay,
    ]))?;
    if updating {
        qemu_img(Command::new("qemu-img").current_dir(&stage.0).args([
            "rebase",
            "-q",
            "-f",
            "qcow2",
            "-F",
            "raw",
            "-b",
            "base.ext2",
            overlay,
        ]))?;
        fs::remove_file(&merged)?;
    }
    qemu_img(
        Command::new("qemu-img")
            .current_dir(&stage.0)
            .args(["check", "-q", "-f", "qcow2", overlay]),
    )?;
    File::open(stage.0.join(overlay))?.sync_all()?;
    if active {
        fs::write(
            stage.0.join("previous"),
            fs::read_link(&current)?.as_os_str().as_encoded_bytes(),
        )?;
    } else if build.join("run-root.ext2").try_exists()? {
        fs::write(stage.0.join("previous"), "original")?;
        let original = state.join("original.img");
        if !original.try_exists()? {
            link_once(&build.join("image.img"), &original)?;
        }
    }
    File::open(&stage.0)?.sync_all()?;
    let generation = (1u64..)
        .map(|n| format!("generation-{n}"))
        .find(|name| !state.join(name).exists())
        .unwrap();
    fs::rename(&stage.0, state.join(&generation))?;
    select(&state, "current", &generation)?;
    println!("selected {generation}");
    Ok(())
}

pub fn rollback(build: &Path) -> io::Result<()> {
    let _session = session(build)?;
    let state = build.join("system");
    let current = state.join("current");
    let previous = fs::read_to_string(current.join("previous"))
        .map(PathBuf::from)
        .map_err(|e| io::Error::other(format!("cannot find previous installation: {e}")))?;
    if previous == Path::new("original") {
        File::open(build.join("run-root.ext2"))?;
        File::open(state.join("original.img"))?;
        fs::remove_file(current)?;
        File::open(&state)?.sync_all()?;
    } else {
        File::open(state.join(&previous).join("data.qcow2"))?;
        select(
            &state,
            "current",
            previous
                .to_str()
                .ok_or_else(|| io::Error::other("invalid generation"))?,
        )?;
    }
    println!("selected {}", previous.display());
    Ok(())
}
