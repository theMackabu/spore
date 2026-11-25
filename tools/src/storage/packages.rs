use std::{
    collections::{BTreeMap, BTreeSet},
    io,
};

fn packages(db: &[u8]) -> io::Result<BTreeMap<&[u8], &[u8]>> {
    let mut entries = BTreeMap::new();
    let mut rest = db;
    while !rest.is_empty() {
        let end = rest
            .windows(2)
            .position(|s| s == b"\n\n")
            .unwrap_or(rest.len());
        let record = rest[..end].trim_ascii_end();
        rest = rest.get(end + 2..).unwrap_or_default();
        if record.is_empty() {
            continue;
        }
        let name = record
            .split(|&b| b == b'\n')
            .find_map(|s| s.strip_prefix(b"P:"))
            .ok_or_else(|| io::Error::other("invalid APK installed record"))?;
        if entries.insert(name, record).is_some() {
            return Err(io::Error::other("duplicate installed APK record"));
        }
    }
    Ok(entries)
}

fn owned_files(base: &BTreeMap<&[u8], &[u8]>, local: &BTreeMap<&[u8], &[u8]>) -> BTreeSet<Vec<u8>> {
    let mut owned = BTreeSet::new();
    for (&name, &record) in local {
        if base.get(name) == Some(&record) {
            continue;
        }
        let mut dir = &[][..];
        for line in record.split(|&b| b == b'\n') {
            if let Some(value) = line.strip_prefix(b"F:") {
                dir = value;
            }
            if let Some(file) = line.strip_prefix(b"R:") {
                owned.insert(if dir.is_empty() {
                    [b"/", file].concat()
                } else {
                    [b"/", dir, b"/", file].concat()
                });
            }
        }
    }
    owned
}

pub struct PackageMerge {
    pub owned: BTreeSet<Vec<u8>>,
    pub installed: Option<Vec<u8>>,
}
impl PackageMerge {
    pub fn new(old: &[u8], new: &[u8], local: &[u8]) -> io::Result<Self> {
        let base = packages(old)?;
        let current = packages(local)?;
        let owned = owned_files(&base, &current);
        let mut merged = current.clone();
        for (name, record) in &base {
            if current.get(name) == Some(record) {
                merged.remove(name);
            }
        }
        let incoming = packages(new)?;
        for (name, record) in incoming {
            if current.get(name) == base.get(name) {
                merged.insert(name, record);
            }
        }
        let installed = (merged != current).then(|| {
            let mut result = Vec::new();
            for record in merged.values() {
                result.extend_from_slice(record);
                result.extend_from_slice(b"\n\n");
            }
            result
        });
        Ok(Self { owned, installed })
    }
}
