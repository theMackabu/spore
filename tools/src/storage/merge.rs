use super::{
    packages::PackageMerge,
    volume::{Node, Volume},
};
use std::{
    collections::{BTreeMap, BTreeSet},
    io,
    path::Path,
};

type Entries = BTreeMap<Vec<u8>, Node>;

fn directory(volume: &Volume, n: Option<Node>) -> io::Result<Entries> {
    n.filter(|n| n.dir())
        .map(|n| volume.children(n))
        .transpose()
        .map(Option::unwrap_or_default)
}

fn database(fs: &Volume) -> io::Result<Vec<u8>> {
    let mut n = fs.node(2)?;
    for name in [b"lib".as_slice(), b"apk", b"db", b"installed"] {
        let Some(child) = directory(fs, Some(n))?.remove(name) else {
            return Ok(Vec::new());
        };
        n = child;
    }
    if n.mode >> 12 != 8 {
        return Err(io::Error::other("invalid installed package database"));
    }
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(n.size as usize)
        .map_err(io::Error::other)?;
    bytes.resize(n.size as usize, 0);
    fs.read(n, 0, &mut bytes)?;
    Ok(bytes)
}

struct Merge<'a> {
    base: &'a Volume,
    incoming: &'a Volume,
    local: &'a Volume,
    packages: PackageMerge,
}

impl Merge<'_> {
    fn same_tree(
        &self,
        a: &Volume,
        n: Option<Node>,
        b: &Volume,
        m: Option<Node>,
        path: &[u8],
        protect: bool,
    ) -> io::Result<bool> {
        if protect && self.packages.owned.contains(path) {
            return Ok(false);
        }
        let (Some(n), Some(m)) = (n, m) else {
            return Ok(n.is_none() && m.is_none());
        };
        if !a.equal(n, b, m)? {
            return Ok(false);
        }
        if n.dir() {
            let left = a.children(n)?;
            let right = b.children(m)?;
            if left.len() != right.len() {
                return Ok(false);
            }
            for (name, child) in left {
                let key = [path, b"/", &name].concat();
                if !self.same_tree(a, Some(child), b, right.get(&name).copied(), &key, protect)? {
                    return Ok(false);
                }
            }
        }
        Ok(true)
    }
    fn copy_tree(&self, parent: Node, name: &[u8], n: Node) -> io::Result<()> {
        let copy = self.local.copy(parent, name, self.incoming, n)?;
        if n.dir() {
            for (key, child) in self.incoming.children(n)? {
                self.copy_tree(copy, &key, child)?;
            }
        }
        Ok(())
    }
    fn remove_tree(&self, parent: Node, name: &[u8], n: Node) -> io::Result<()> {
        if n.dir() {
            for (key, child) in self.local.children(n)? {
                self.remove_tree(n, &key, child)?;
            }
        }
        self.local.remove(parent, name)
    }
    fn merge_dir(&self, path: &[u8], base: Option<Node>, new: Node, local: Node) -> io::Result<()> {
        let before = directory(self.base, base)?;
        let next = self.incoming.children(new)?;
        let current = self.local.children(local)?;
        let mut used: BTreeSet<_> = next.keys().chain(current.keys()).cloned().collect();
        for name in before.keys().chain(next.keys()).collect::<BTreeSet<_>>() {
            let key = [path, b"/", name].concat();
            let old = before.get(name).copied();
            let inc = next.get(name).copied();
            let here = current.get(name).copied();
            let mut apply = || -> io::Result<()> {
                if let (Some(i), Some(h)) = (inc.filter(|n| n.dir()), here.filter(|n| n.dir())) {
                    if let Some(o) = old.filter(|n| n.dir()) {
                        if h.metadata() == o.metadata() && h.metadata() != i.metadata() {
                            self.local.metadata(h, i)?;
                        } else if h.metadata() != i.metadata() && o.metadata() != i.metadata() {
                            eprintln!(
                                "kept local permissions on {} (base mode {:o}, uid {}, gid {})",
                                String::from_utf8_lossy(&key),
                                i.mode & 0o7777,
                                i.uid,
                                i.gid
                            );
                        }
                    }
                    return self.merge_dir(&key, old, i, h);
                }
                if self.same_tree(self.base, old, self.incoming, inc, &key, false)?
                    || self.same_tree(self.local, here, self.incoming, inc, &key, false)?
                {
                    return Ok(());
                }
                if key == b"/lib/apk/db/installed"
                    && let Some(bytes) = &self.packages.installed
                    && let Some(n) = here.filter(|n| n.mode >> 12 == 8 && n.links == 1)
                    && old.is_some()
                    && inc.is_some()
                {
                    return self.local.replace_contents(n, bytes);
                }
                if self.same_tree(self.local, here, self.base, old, &key, true)? {
                    if let Some(h) = here {
                        self.remove_tree(local, name, h)?;
                    }
                    if let Some(i) = inc {
                        self.copy_tree(local, name, i)?;
                    }
                } else if let Some(i) = inc {
                    for suffix in 0u32.. {
                        let ext = if suffix == 0 {
                            ".spore-new".into()
                        } else {
                            format!(".spore-new.{suffix}")
                        };
                        let candidate =
                            [&name[..name.len().min(255 - ext.len())], ext.as_bytes()].concat();
                        if used.insert(candidate.clone()) {
                            self.copy_tree(local, &candidate, i)?;
                            eprintln!(
                                "kept local {}; incoming copy at {}",
                                String::from_utf8_lossy(&key),
                                String::from_utf8_lossy(&[path, b"/", &candidate].concat())
                            );
                            break;
                        }
                    }
                } else {
                    eprintln!(
                        "kept local {} (removed from base)",
                        String::from_utf8_lossy(&key)
                    );
                }
                Ok(())
            };
            apply()
                .map_err(|e| io::Error::other(format!("{}: {e}", String::from_utf8_lossy(&key))))?;
        }
        Ok(())
    }
}

pub fn apply(previous: Option<&Path>, new: &Path, staged: &Path) -> io::Result<()> {
    let incoming = Volume::open(new, false)?;
    let previous = previous.map(|p| Volume::open(p, false)).transpose()?;
    let local = Volume::open(staged, true)?;
    let base_db = previous
        .as_ref()
        .map(database)
        .transpose()?
        .unwrap_or_default();
    let packages = PackageMerge::new(&base_db, &database(&incoming)?, &database(&local)?)?;
    let merge = Merge {
        base: previous.as_ref().unwrap_or(&incoming),
        incoming: &incoming,
        local: &local,
        packages,
    };
    merge.merge_dir(
        b"",
        previous.as_ref().map(|fs| fs.node(2)).transpose()?,
        incoming.node(2)?,
        local.node(2)?,
    )?;
    local.flush()
}
