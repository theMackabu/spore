use core::ffi::c_void;

const EHDR: usize = 64;
const PHDR: usize = 56;
const ET_EXEC: u16 = 2;
const ET_DYN: u16 = 3;
const EM_AARCH64: u16 = 183;
const PT_LOAD: u32 = 1;
const PT_INTERP: u32 = 3;
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
    kind: u16,
    entry: u64,
    table: u64,
    count: u16,
}
impl Header {
    fn read(r: &Reader) -> Option<Self> {
        let b = r.bytes::<EHDR>(0)?;
        let kind = le16(&b, 16);
        if &b[..6] != b"\x7fELF\x02\x01"
            || !matches!(kind, ET_EXEC | ET_DYN)
            || le16(&b, 18) != EM_AARCH64
            || usize::from(le16(&b, 54)) != PHDR
        {
            return None;
        }
        let header = Self {
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
    fn segments<'a>(&'a self, r: &'a Reader) -> impl Iterator<Item = Option<Segment>> + 'a {
        (0..u64::from(self.count)).map(|i| {
            let b = r.bytes::<PHDR>(self.table + i * PHDR as u64)?;
            Some(Segment {
                kind: le32(&b, 0),
                flags: le32(&b, 4),
                offset: le64(&b, 8),
                vaddr: le64(&b, 16),
                filesz: le64(&b, 32),
                memsz: le64(&b, 40),
                align: le64(&b, 48),
            })
        })
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
#[repr(C)]
pub struct Loaded {
    entry: u64,
    load_base: u64,
    phdr: u64,
    brk_base: u64,
    phent: u16,
    phnum: u16,
}

fn load(
    reader: &Reader,
    load_base: u64,
    mut map: impl FnMut(&Segment, u64, u64, u64) -> bool,
) -> Option<Loaded> {
    let h = Header::read(reader)?;
    let base = if h.kind == ET_DYN { load_base } else { 0 };
    let mut out = Loaded {
        entry: base.checked_add(h.entry)?,
        load_base: base,
        phdr: 0,
        brk_base: 0,
        phent: PHDR as u16,
        phnum: h.count,
    };
    let table_end = h.table_end()?;
    for segment in h.segments(reader) {
        let p = segment?;
        if p.kind != PT_LOAD {
            continue;
        }
        let file_end = p.offset.checked_add(p.filesz)?;
        if p.memsz < p.filesz
            || file_end > reader.size
            || (p.align > PAGE && p.vaddr % p.align != p.offset % p.align)
        {
            return None;
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
    }
    (out.brk_base != 0).then_some(out)
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
) -> bool {
    let (Some(read), Some(map)) = (read, map) else {
        return false;
    };
    if out.is_null() {
        return false;
    }
    let r = Reader { read, ctx, size };
    match load(&r, base, |p, a, s, e| unsafe { map(map_ctx, p, a, s, e) }) {
        Some(value) => {
            unsafe { out.write(value) };
            true
        }
        None => false,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_elf_find_interp(
    read: Option<Read>,
    ctx: *mut c_void,
    size: u64,
    out: *mut u8,
    cap: usize,
) -> bool {
    let Some(read) = read else { return false };
    if out.is_null() || cap == 0 {
        return false;
    }
    let reader = Reader { read, ctx, size };
    let Some(header) = Header::read(&reader) else {
        return false;
    };
    for segment in header.segments(&reader) {
        let Some(p) = segment else { return false };
        if p.kind != PT_INTERP {
            continue;
        }
        if p.filesz == 0
            || p.filesz >= cap as u64
            || p.offset.checked_add(p.filesz).is_none_or(|end| end > size)
        {
            return false;
        }
        let count = p.filesz as usize;
        return unsafe { read(ctx, p.offset, out, count) && out.add(count - 1).read() == 0 };
    }
    false
}
