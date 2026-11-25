use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs, io,
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
    process::Command,
};
use tempfile::{NamedTempFile, tempdir_in};

#[derive(Deserialize)]
pub struct Archive {
    pub url: String,
    pub sha256: String,
}

#[derive(Deserialize)]
pub struct Package {
    #[serde(flatten)]
    pub archive: Archive,
    pub files: Vec<PathBuf>,
    pub manpages: Option<PathBuf>,
}

#[derive(Deserialize)]
pub struct Catalog {
    pub packages: Vec<Package>,
    pub sdks: BTreeMap<String, Vec<Archive>>,
}

pub fn catalog() -> Catalog {
    serde_json::from_str(include_str!("../../../userland/prebuilt/packages.json"))
        .expect("invalid package catalog")
}

fn run(cmd: &mut Command) -> io::Result<()> {
    let status = cmd
        .status()
        .map_err(|e| io::Error::new(e.kind(), format!("{cmd:?}: {e}")))?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!("{cmd:?}: {status}")))
    }
}

fn digest(path: &Path) -> io::Result<String> {
    let mut hash = Sha256::new();
    io::copy(&mut fs::File::open(path)?, &mut hash)?;
    Ok(format!("{:x}", hash.finalize()))
}

impl Archive {
    pub fn fetch(&self, cache: &Path) -> io::Result<PathBuf> {
        fs::create_dir_all(cache)?;
        let path = cache.join(&self.sha256);
        if path.is_file() && digest(&path)? == self.sha256 {
            return Ok(path);
        }
        let tmp = NamedTempFile::new_in(cache)?;
        run(Command::new("curl")
            .args(["-fsSL", "--retry", "4", "-o"])
            .arg(tmp.path())
            .arg(&self.url))?;
        if digest(tmp.path())? != self.sha256 {
            return Err(io::Error::other(format!("SHA-256 mismatch: {}", self.url)));
        }
        tmp.persist(&path)?;
        Ok(path)
    }

    pub fn extract(&self, cache: &Path, dest: &Path, members: &[PathBuf]) -> io::Result<()> {
        fs::create_dir_all(dest)?;
        let src = self.fetch(cache)?;
        let tmp = tempdir_in(dest)?;
        run(Command::new("bsdtar")
            .arg("-xf")
            .arg(src)
            .arg("-C")
            .arg(tmp.path())
            .arg("--")
            .args(members))?;
        for member in members {
            let file = tmp.path().join(member);
            if !fs::symlink_metadata(&file)?.is_file() {
                return Err(io::Error::other(format!(
                    "expected regular package member {}",
                    member.display()
                )));
            }
            if self.url.ends_with(".zip") {
                fs::set_permissions(&file, fs::Permissions::from_mode(0o755))?;
            }
            fs::rename(file, dest.join(member.file_name().unwrap()))?;
        }
        Ok(())
    }
}

pub fn prepare_userland(cache: &Path, dest: &Path) -> io::Result<()> {
    let catalog = catalog();
    for package in &catalog.packages {
        if !package.files.is_empty() {
            package.archive.extract(cache, dest, &package.files)?;
        }
    }
    let manuals = dest.join("manuals");
    let stamp = dest.join("manuals.stamp");
    let sources: Vec<_> = catalog
        .packages
        .iter()
        .filter_map(|p| p.manpages.as_ref().map(|dir| (&p.archive, dir)))
        .collect();
    let pins: String = sources
        .iter()
        .map(|(a, dir)| format!("{} {}\n", a.sha256, dir.display()))
        .collect();
    if !manuals.is_dir() || fs::read_to_string(&stamp).ok().as_ref() != Some(&pins) {
        let tmp = tempdir_in(dest)?;
        for (archive, dir) in sources {
            run(Command::new("bsdtar")
                .arg("-xf")
                .arg(archive.fetch(cache)?)
                .arg("-C")
                .arg(tmp.path())
                .arg("--")
                .arg(dir))?;
        }
        if manuals.exists() {
            fs::remove_dir_all(&manuals)?;
        }
        fs::rename(tmp.path().join("usr/share/man"), &manuals)?;
        fs::write(stamp, pins)?;
    }
    for (name, archives) in catalog.sdks {
        let stamp = dest.join(format!("{name}-sdk.stamp"));
        let pins: String = archives.iter().map(|a| format!("{}\n", a.sha256)).collect();
        let sdk = dest.join(format!("{name}-sdk"));
        if sdk.is_dir() && fs::read_to_string(&stamp).ok().as_ref() == Some(&pins) {
            continue;
        }
        let tmp = tempdir_in(dest)?;
        for archive in archives {
            run(Command::new("bsdtar")
                .arg("-xf")
                .arg(archive.fetch(cache)?)
                .arg("-C")
                .arg(tmp.path())
                .arg("usr"))?;
        }
        if sdk.exists() {
            fs::remove_dir_all(&sdk)?;
        }
        fs::rename(tmp.path(), sdk)?;
        fs::write(stamp, pins)?;
    }
    Ok(())
}
