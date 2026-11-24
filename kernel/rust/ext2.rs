#[repr(C)]
pub struct SporeExt2DirRecord {
    pub ino: u32,
    pub rec_len: u16,
    pub name_len: u8,
    pub kind: u8,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_ext2_parse_dir_record(
    data: *const u8,
    available: usize,
    out: *mut SporeExt2DirRecord,
) -> bool {
    if data.is_null() || out.is_null() || available < 8 { return false; }
    let bytes = unsafe { core::slice::from_raw_parts(data, available) };
    let ino = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    let rec_len = u16::from_le_bytes([bytes[4], bytes[5]]) as usize;
    let name_len = bytes[6] as usize;
    if rec_len < 8 || rec_len & 3 != 0 || rec_len > available || (ino != 0 && (name_len == 0 || name_len + 8 > rec_len)) {
        return false;
    }
    unsafe { *out = SporeExt2DirRecord { ino, rec_len: rec_len as u16, name_len: name_len as u8, kind: bytes[7] }; }
    true
}
