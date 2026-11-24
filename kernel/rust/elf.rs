#[repr(C)]
pub struct SporeElfHeader { pub kind: u16, pub phentsize: u16, pub phnum: u16, pub entry: u64, pub phoff: u64 }
#[repr(C)]
pub struct SporeElfProgram { pub kind: u32, pub flags: u32, pub offset: u64, pub vaddr: u64, pub filesz: u64, pub memsz: u64, pub align: u64 }

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
