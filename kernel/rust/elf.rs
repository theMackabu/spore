#[repr(C)]
pub struct SporeElfHeader { pub kind: u16, pub phentsize: u16, pub phnum: u16, pub entry: u64, pub phoff: u64 }
#[repr(C)]
pub struct SporeElfProgram { pub kind: u32, pub flags: u32, pub offset: u64, pub vaddr: u64, pub filesz: u64, pub memsz: u64, pub align: u64 }
#[repr(C)]
pub struct SporeElfLoaded { pub entry: u64, pub load_base: u64, pub phdr: u64, pub brk_base: u64, pub phent: u16, pub phnum: u16 }
type Read = unsafe extern "C" fn(*mut core::ffi::c_void, u64, *mut u8, usize) -> bool;
type Map = unsafe extern "C" fn(*mut core::ffi::c_void, *const SporeElfProgram, u64, u64, u64) -> bool;

fn u16_at(b: &[u8], n: usize) -> u16 { u16::from_le_bytes([b[n], b[n + 1]]) }
fn u32_at(b: &[u8], n: usize) -> u32 { u32::from_le_bytes([b[n], b[n + 1], b[n + 2], b[n + 3]]) }
fn u64_at(b: &[u8], n: usize) -> u64 { u64::from_le_bytes([b[n], b[n + 1], b[n + 2], b[n + 3], b[n + 4], b[n + 5], b[n + 6], b[n + 7]]) }

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_elf_parse_header(data: *const u8, len: usize, image_size: u64, out: *mut SporeElfHeader) -> bool {
    if data.is_null() || out.is_null() || len < 64 || image_size < 64 { return false; }
    let b = unsafe { core::slice::from_raw_parts(data, len) };
    if b[0..4] != [0x7f, b'E', b'L', b'F'] || b[4] != 2 || b[5] != 1 || !(u16_at(b, 16) == 2 || u16_at(b, 16) == 3) || u16_at(b, 18) != 183 || u16_at(b, 54) != 56 { return false; }
    let phoff = u64_at(b, 32); let phnum = u16_at(b, 56) as u64;
    if phoff > image_size || phnum > (image_size - phoff) / 56 { return false; }
    unsafe { *out = SporeElfHeader { kind: u16_at(b, 16), phentsize: 56, phnum: phnum as u16, entry: u64_at(b, 24), phoff }; }
    true
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_elf_parse_program(data: *const u8, len: usize, out: *mut SporeElfProgram) -> bool {
    if data.is_null() || out.is_null() || len < 56 { return false; }
    let b = unsafe { core::slice::from_raw_parts(data, len) };
    unsafe { *out = SporeElfProgram { kind: u32_at(b, 0), flags: u32_at(b, 4), offset: u64_at(b, 8), vaddr: u64_at(b, 16), filesz: u64_at(b, 32), memsz: u64_at(b, 40), align: u64_at(b, 48) }; }
    true
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_elf_interp_valid(data: *const u8, len: usize) -> bool {
    !data.is_null() && len != 0 && unsafe { *data.add(len - 1) } == 0
}

fn bounds(off: u64, len: u64, size: u64) -> bool { off <= size && len <= size - off }
fn read_header(read: Read, ctx: *mut core::ffi::c_void, size: u64) -> Option<SporeElfHeader> {
    let mut bytes = [0u8; 64];
    if size < 64 || !unsafe { read(ctx, 0, bytes.as_mut_ptr(), 64) } { return None; }
    let mut header = SporeElfHeader { kind: 0, phentsize: 0, phnum: 0, entry: 0, phoff: 0 };
    if !unsafe { spore_elf_parse_header(bytes.as_ptr(), bytes.len(), size, &mut header) } { return None; }
    Some(header)
}
fn read_program(read: Read, ctx: *mut core::ffi::c_void, off: u64, size: u64) -> Option<SporeElfProgram> {
    let mut bytes = [0u8; 56]; let mut program = SporeElfProgram { kind: 0, flags: 0, offset: 0, vaddr: 0, filesz: 0, memsz: 0, align: 0 };
    if !bounds(off, 56, size) || !unsafe { read(ctx, off, bytes.as_mut_ptr(), 56) } || !unsafe { spore_elf_parse_program(bytes.as_ptr(), 56, &mut program) } { return None; }
    Some(program)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_elf_load(read: Option<Read>, ctx: *mut core::ffi::c_void, size: u64, load_base: u64, map: Option<Map>, map_ctx: *mut core::ffi::c_void, out: *mut SporeElfLoaded) -> bool {
    let (read, map) = match (read, map) { (Some(read), Some(map)) if !out.is_null() => (read, map), _ => return false };
    let header = match read_header(read, ctx, size) { Some(header) => header, None => return false };
    let base = if header.kind == 3 { load_base } else { 0 }; let mut high = 0; let mut phdr = 0;
    let phend = match header.phoff.checked_add(header.phnum as u64 * 56) { Some(value) => value, None => return false };
    for index in 0..header.phnum as u64 {
        let program = match read_program(read, ctx, header.phoff + index * 56, size) { Some(program) => program, None => return false };
        if program.kind != 1 { continue; }
        if program.memsz < program.filesz || !bounds(program.offset, program.filesz, size) || (program.align > 4096 && program.vaddr % program.align != program.offset % program.align) { return false; }
        let segment = match base.checked_add(program.vaddr) { Some(value) => value, None => return false };
        let segment_end = match segment.checked_add(program.memsz) { Some(value) => value, None => return false };
        let end = match segment_end.checked_add(4095) { Some(value) => value & !4095, None => return false };
        if !unsafe { map(map_ctx, &program, segment, segment & !4095, end) } { return false; }
        if segment_end > high { high = segment_end; }
        if header.phoff >= program.offset && phend <= program.offset.saturating_add(program.filesz) { phdr = segment + header.phoff - program.offset; }
    }
    if high == 0 { return false; }
    let brk = match high.checked_add(4095) { Some(value) => value & !4095, None => return false };
    unsafe { *out = SporeElfLoaded { entry: base + header.entry, load_base: base, phdr, brk_base: brk, phent: 56, phnum: header.phnum }; }
    true
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_elf_find_interp(read: Option<Read>, ctx: *mut core::ffi::c_void, size: u64, out: *mut u8, cap: usize) -> bool {
    let read = match read { Some(read) if !out.is_null() && cap != 0 => read, _ => return false };
    let header = match read_header(read, ctx, size) { Some(header) => header, None => return false };
    for index in 0..header.phnum as u64 {
        let program = match read_program(read, ctx, header.phoff + index * 56, size) { Some(program) => program, None => return false };
        if program.kind == 3 {
            return program.filesz != 0 && (program.filesz as usize) < cap && bounds(program.offset, program.filesz, size) && unsafe { read(ctx, program.offset, out, program.filesz as usize) } && unsafe { *out.add(program.filesz as usize - 1) == 0 };
        }
    }
    false
}
