use std::{
    collections::BTreeMap,
    ffi::{CString, c_char, c_void},
    io,
    ptr::NonNull,
};
#[cfg(not(target_arch = "wasm32"))]
use std::{
    fs::{File, OpenOptions},
    os::fd::AsRawFd,
    path::Path,
};

#[path = "../../../kernel/fs/ext2/bitmap.rs"]
mod bitmap;
#[path = "../../../kernel/fs/ext2/records.rs"]
mod records;

#[derive(Clone, Copy, Default)]
#[repr(C, align(8))]
pub struct Node {
    pub ino: u32,
    pub mode: u16,
    pub links: u16,
    pub uid: u16,
    pub gid: u16,
    pub size: u32,
    times_and_blocks: [u32; 19],
}
impl Node {
    pub fn dir(self) -> bool {
        self.mode >> 12 == 4
    }
    pub fn metadata(self) -> (u16, u16, u16) {
        (self.mode, self.uid, self.gid)
    }
}
#[repr(C)]
struct Entry {
    ino: u64,
    kind: u8,
    len: u8,
    name: [u8; 256],
}

type Fs = *mut c_void;
unsafe extern "C" {
    fn volume_open(fd: i32, writable: bool) -> Fs;
    fn volume_close(fs: Fs);
    fn ext2_flush(fs: Fs) -> bool;
    fn ext2_inode(fs: Fs, ino: u32, node: *mut Node) -> bool;
    fn ext2_next_dirent(fs: Fs, node: &Node, cursor: &mut u64, entry: *mut Entry) -> i32;
    fn ext2_read_file(fs: Fs, node: &Node, off: u64, buf: *mut u8, len: u32, got: &mut u32)
    -> bool;
    fn ext2_readlink_node(fs: Fs, node: &Node, buf: *mut u8, cap: usize, got: &mut usize) -> bool;
    fn volume_copy_entry(
        src: Fs,
        ino: u32,
        dst: Fs,
        parent: u32,
        path: *const c_char,
        created: &mut u32,
    ) -> bool;
    fn ext2_remove_from(fs: Fs, parent: u32, path: *const c_char) -> i32;
    fn ext2_write_file(fs: Fs, node: &mut Node, off: u64, src: *const u8, len: u64) -> i64;
    fn ext2_truncate(fs: Fs, node: &mut Node, size: u64) -> bool;
    fn ext2_chmod_node(fs: Fs, node: &Node, mode: u32) -> bool;
    fn ext2_chown_node(fs: Fs, node: &Node, uid: u32, gid: u32) -> bool;
}
fn check(ok: bool) -> io::Result<()> {
    if ok {
        Ok(())
    } else {
        Err(io::Error::other("ext2 filesystem operation failed"))
    }
}
fn entry_path(name: &[u8]) -> io::Result<CString> {
    if name.is_empty() || name.len() > 255 || name.contains(&b'/') || name == b"." || name == b".."
    {
        return Err(io::Error::other("invalid ext2 directory entry"));
    }
    CString::new([b"/", name].concat()).map_err(io::Error::other)
}

pub struct Volume {
    fs: NonNull<c_void>,
    #[cfg(not(target_arch = "wasm32"))]
    file: File,
}
impl Volume {
    #[cfg(target_arch = "wasm32")]
    pub fn device(id: i32, writable: bool) -> io::Result<Self> {
        let fs = NonNull::new(unsafe { volume_open(id, writable) })
            .ok_or_else(|| io::Error::other("invalid ext2 upgrade image"))?;
        Ok(Self { fs })
    }
    #[cfg(not(target_arch = "wasm32"))]
    pub fn open(path: &Path, writable: bool) -> io::Result<Self> {
        let file = OpenOptions::new().read(true).write(writable).open(path)?;
        let fs = NonNull::new(unsafe { volume_open(file.as_raw_fd(), writable) })
            .ok_or_else(|| io::Error::other(format!("invalid ext2 volume {}", path.display())))?;
        Ok(Self { fs, file })
    }
    pub fn flush(&self) -> io::Result<()> {
        check(unsafe { ext2_flush(self.fs.as_ptr()) })?;
        #[cfg(not(target_arch = "wasm32"))]
        self.file.sync_all()?;
        Ok(())
    }
    pub fn node(&self, ino: u32) -> io::Result<Node> {
        let mut n = Node::default();
        check(unsafe { ext2_inode(self.fs.as_ptr(), ino, &mut n) })?;
        Ok(n)
    }
    pub fn children(&self, dir: Node) -> io::Result<BTreeMap<Vec<u8>, Node>> {
        let mut entries = BTreeMap::new();
        let mut cursor = 0;
        loop {
            let mut ent = Entry {
                ino: 0,
                kind: 0,
                len: 0,
                name: [0; 256],
            };
            match unsafe { ext2_next_dirent(self.fs.as_ptr(), &dir, &mut cursor, &mut ent) } {
                0 => return Ok(entries),
                1 => {
                    let name = &ent.name[..ent.len as usize];
                    entry_path(name)?;
                    check(
                        entries
                            .insert(name.to_vec(), self.node(ent.ino as u32)?)
                            .is_none(),
                    )?;
                }
                _ => return Err(io::Error::other("cannot read ext2 directory")),
            }
        }
    }
    pub fn read(&self, n: Node, offset: usize, buf: &mut [u8]) -> io::Result<()> {
        let mut got = 0;
        check(
            unsafe {
                ext2_read_file(
                    self.fs.as_ptr(),
                    &n,
                    offset as u64,
                    buf.as_mut_ptr(),
                    buf.len() as u32,
                    &mut got,
                )
            } && got as usize == buf.len(),
        )
    }
    fn link(&self, n: Node) -> io::Result<Vec<u8>> {
        check(n.size < 65536)?;
        let mut buf = vec![0; n.size as usize + 1];
        let mut got = 0;
        check(
            unsafe {
                ext2_readlink_node(self.fs.as_ptr(), &n, buf.as_mut_ptr(), buf.len(), &mut got)
            } && got == n.size as usize,
        )?;
        buf.truncate(got);
        Ok(buf)
    }
    pub fn equal(&self, n: Node, other: &Self, m: Node) -> io::Result<bool> {
        if n.metadata() != m.metadata() || n.links != m.links && !n.dir() {
            return Ok(false);
        }
        if n.dir() {
            return Ok(true);
        }
        if n.size != m.size {
            return Ok(false);
        }
        if n.mode >> 12 == 10 {
            return Ok(self.link(n)? == other.link(m)?);
        }
        let (mut a, mut b) = ([0; 16384], [0; 16384]);
        for off in (0..n.size as usize).step_by(a.len()) {
            let len = a.len().min(n.size as usize - off);
            self.read(n, off, &mut a[..len])?;
            other.read(m, off, &mut b[..len])?;
            if a[..len] != b[..len] {
                return Ok(false);
            }
        }
        Ok(true)
    }
    pub fn copy(&self, parent: Node, name: &[u8], src: &Self, n: Node) -> io::Result<Node> {
        let path = entry_path(name)?;
        let mut ino = 0;
        check(unsafe {
            volume_copy_entry(
                src.fs.as_ptr(),
                n.ino,
                self.fs.as_ptr(),
                parent.ino,
                path.as_ptr(),
                &mut ino,
            )
        })?;
        self.node(ino)
    }
    pub fn replace_contents(&self, mut n: Node, bytes: &[u8]) -> io::Result<()> {
        check(
            unsafe { ext2_truncate(self.fs.as_ptr(), &mut n, 0) }
                && unsafe {
                    ext2_write_file(
                        self.fs.as_ptr(),
                        &mut n,
                        0,
                        bytes.as_ptr(),
                        bytes.len() as u64,
                    )
                } == bytes.len() as i64,
        )
    }
    pub fn remove(&self, parent: Node, name: &[u8]) -> io::Result<()> {
        let path = entry_path(name)?;
        check(unsafe { ext2_remove_from(self.fs.as_ptr(), parent.ino, path.as_ptr()) } == 0)
    }
    pub fn metadata(&self, n: Node, src: Node) -> io::Result<()> {
        check(unsafe {
            ext2_chown_node(self.fs.as_ptr(), &n, src.uid.into(), src.gid.into())
                && ext2_chmod_node(self.fs.as_ptr(), &n, src.mode.into())
        })
    }
}
impl Drop for Volume {
    fn drop(&mut self) {
        unsafe { volume_close(self.fs.as_ptr()) };
    }
}
