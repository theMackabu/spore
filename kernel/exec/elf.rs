use core::ffi::c_void;

const EHDR: usize = 64;
const PHDR: usize = 56;
const ET_EXEC: u16 = 2;
const ET_DYN: u16 = 3;
const MACHINE: u16 = if cfg!(all(target_os = "none", target_arch = "riscv64")) {
    243
} else if cfg!(all(target_os = "none", target_arch = "x86_64")) {
    62
} else {
    183
};
const PT_LOAD: u32 = 1;
const PT_INTERP: u32 = 3;
const PT_NOTE: u32 = 4;
const PAGE: u64 = 4096;

type Read = unsafe extern "C" fn(*mut c_void, u64, *mut u8, usize) -> bool;
type Map = unsafe extern "C" fn(*mut c_void, &Segment, u64, u64, u64) -> bool;

struct Reader {
    read: Read,
    ctx: *mut c_void,
    size: u64,
}
impl Reader {
    fn read_at(&self, offset: u64, dst: &mut [u8]) -> Option<()> {
        if offset.checked_add(dst.len() as u64)? > self.size {
            return None;
        }
        unsafe { (self.read)(self.ctx, offset, dst.as_mut_ptr(), dst.len()) }.then_some(())
    }
    fn bytes<const N: usize>(&self, offset: u64) -> Option<[u8; N]> {
        let mut bytes = [0; N];
        self.read_at(offset, &mut bytes)?;
        Some(bytes)
    }
}

fn le16(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes(b[i..i + 2].try_into().unwrap())
}
fn le32(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(b[i..i + 4].try_into().unwrap())
}
fn le64(b: &[u8], i: usize) -> u64 {
    u64::from_le_bytes(b[i..i + 8].try_into().unwrap())
}

struct Header {
    osabi: u8,
    kind: u16,
    entry: u64,
    table: u64,
    count: u16,
}
impl Header {
    fn read(r: &Reader) -> Option<Self> {
        let b = r.bytes::<EHDR>(0)?;
        let kind = le16(&b, 16);
        if &b[..7] != b"\x7fELF\x02\x01\x01"
            || !matches!(b[7], 0 | 3 | 9)
            || !matches!(kind, ET_EXEC | ET_DYN)
            || le16(&b, 18) != MACHINE
            || le32(&b, 20) != 1
            || usize::from(le16(&b, 52)) != EHDR
            || usize::from(le16(&b, 54)) != PHDR
        {
            return None;
        }
        let header = Self {
            osabi: b[7],
            kind,
            entry: le64(&b, 24),
            table: le64(&b, 32),
            count: le16(&b, 56),
        };
        (header.table_end()? <= r.size).then_some(header)
    }
    fn table_end(&self) -> Option<u64> {
        self.table.checked_add(u64::from(self.count) * PHDR as u64)
    }
    fn visit_segments(
        &self,
        r: &Reader,
        mut visit: impl FnMut(Segment) -> Option<()>,
    ) -> Option<()> {
        let mut offset = self.table;
        let end = self.table_end()?;
        let mut batch = [0; PHDR * 8];
        while offset < end {
            let n = (end - offset).min(batch.len() as u64) as usize;
            r.read_at(offset, &mut batch[..n])?;
            for b in batch[..n].as_chunks::<PHDR>().0 {
                visit(Segment {
                    kind: le32(b, 0),
                    flags: le32(b, 4),
                    offset: le64(b, 8),
                    vaddr: le64(b, 16),
                    filesz: le64(b, 32),
                    memsz: le64(b, 40),
                    align: le64(b, 48),
                })?;
            }
            offset += n as u64;
        }
        Some(())
    }
}

#[repr(C)]
pub struct Segment {
    kind: u32,
    flags: u32,
    offset: u64,
    vaddr: u64,
    filesz: u64,
    memsz: u64,
    align: u64,
}
fn freebsd_abi_note(reader: &Reader, note: &Segment) -> bool {
    let Some(end) = note.offset.checked_add(note.filesz).filter(|&n| n <= reader.size) else {
        return false;
    };
    let mut pos = note.offset;
    while end - pos >= 12 {
        let Some(header) = reader.bytes::<12>(pos) else { return false };
        pos += 12;
        let namesz = u64::from(le32(&header, 0));
        let descsz = u64::from(le32(&header, 4));
        let size = namesz.next_multiple_of(4) + descsz.next_multiple_of(4);
        if size > end - pos { return false; }
        if le32(&header, 8) == 1 && namesz == 8 && descsz == 4 && reader.bytes(pos) == Some(*b"FreeBSD\0") {
            return true;
        }
        pos += size;
    }
    false
}

#[repr(C)]
pub struct Loaded {
    entry: u64,
    runtime_entry: u64,
    load_base: u64,
    at_base: u64,
    phdr: u64,
    phent: u16,
    phnum: u16,
    osabi: u8,
    brk_base: u64,
}

fn load(
    reader: &Reader,
    load_base: u64,
    mut map: impl FnMut(&Segment, u64, u64, u64) -> bool,
) -> Option<(Loaded, Option<Segment>)> {
    let h = Header::read(reader)?;
    let base = if h.kind == ET_DYN { load_base } else { 0 };
    let entry = base.checked_add(h.entry)?;
    let mut out = Loaded {
        entry,
        runtime_entry: entry,
        at_base: 0,
        load_base: base,
        phdr: 0,
        brk_base: 0,
        phent: PHDR as u16,
        phnum: h.count,
        osabi: h.osabi,
    };
    let table_end = h.table_end()?;
    let mut interpreter = None;
    h.visit_segments(reader, |p| {
        if p.kind == PT_NOTE && out.osabi == 0 && p.align <= 4 && freebsd_abi_note(reader, &p) {
            out.osabi = 9;
        }
        if p.kind == PT_INTERP {
            if interpreter.replace(p).is_some() {
                return None;
            }
            return Some(());
        }
        if p.kind != PT_LOAD {
            return Some(());
        }
        let file_end = p.offset.checked_add(p.filesz)?;
        if p.memsz < p.filesz
            || file_end > reader.size
            || (p.align > 1
                && (!p.align.is_power_of_two() || (p.vaddr ^ p.offset) & (p.align - 1) != 0))
        {
            return None;
        }
        if p.memsz == 0 {
            return Some(());
        }
        let addr = base.checked_add(p.vaddr)?;
        let end = addr.checked_add(p.memsz)?.checked_add(PAGE - 1)? & !(PAGE - 1);
        if !map(&p, addr, addr & !(PAGE - 1), end) {
            return None;
        }
        out.brk_base = out.brk_base.max(end);
        if h.table >= p.offset && table_end <= file_end {
            out.phdr = addr.checked_add(h.table - p.offset)?;
        }
        Some(())
    })?;
    (out.brk_base != 0).then_some((out, interpreter))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_elf_load(
    read: Option<Read>,
    ctx: *mut c_void,
    size: u64,
    base: u64,
    map: Option<Map>,
    map_ctx: *mut c_void,
    out: *mut Loaded,
    path: *mut u8,
    cap: usize,
) -> bool {
    let (Some(read), Some(map)) = (read, map) else {
        return false;
    };
    if out.is_null() {
        return false;
    }
    let r = Reader { read, ctx, size };
    let Some((loaded, interpreter)) =
        load(&r, base, |p, a, s, e| unsafe { map(map_ctx, p, a, s, e) })
    else {
        return false;
    };
    if !path.is_null() {
        if cap == 0 {
            return false;
        }
        if let Some(p) = interpreter {
            if p.filesz < 2
                || p.filesz > cap as u64
                || p.offset.checked_add(p.filesz).is_none_or(|end| end > size)
            {
                return false;
            }
            let bytes = p.filesz as usize;
            if !unsafe { read(ctx, p.offset, path, bytes) } {
                return false;
            }
            let name = unsafe { core::slice::from_raw_parts(path, bytes) };
            if core::ffi::CStr::from_bytes_with_nul(name).is_err() {
                return false;
            }
        } else {
            unsafe { path.write(0) };
        }
    }
    unsafe { out.write(loaded) };
    true
}
