fn first_free(bytes: &[u8], start: u32, end: u32) -> Option<u32> {
    if start >= end {
        return None;
    }
    let first = (start / 8) as usize;
    let range = bytes.get(first..end.div_ceil(8) as usize)?;
    let mut available = !range[0] & (u8::MAX << (start % 8));
    let mut index = first;
    if available == 0 {
        let next = range[1..].iter().position(|&byte| byte != u8::MAX)? + 1;
        index += next;
        available = !range[next];
    }
    let bit = index as u32 * 8 + available.trailing_zeros();
    (bit < end).then_some(bit)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ext2_free_bit(data: *const u8, bytes: usize, start: u32, end: u32) -> u32 {
    first_free(
        unsafe { core::slice::from_raw_parts(data, bytes) },
        start,
        end,
    )
    .unwrap_or(u32::MAX)
}
