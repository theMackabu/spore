use core::slice;

fn append_components(path: &[u8], out: &mut [u8], mut len: usize) -> Option<usize> {
    for name in path.split(|&b| b == b'/') {
        match name {
            b"" | b"." => {}
            b".." => {
                len = out[..len]
                    .iter()
                    .rposition(|&b| b == b'/')
                    .unwrap_or(0)
                    .max(1);
            }
            _ => {
                if len > 1 {
                    *out.get_mut(len)? = b'/';
                    len += 1;
                }
                let end = len.checked_add(name.len())?;
                out.get_mut(len..end)?.copy_from_slice(name);
                len = end;
            }
        }
    }
    Some(len)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_normalize_path(
    base: *const u8,
    base_len: usize,
    path: *const u8,
    path_len: usize,
    out: *mut u8,
    cap: usize,
) -> usize {
    let base = unsafe { slice::from_raw_parts(base, base_len) };
    let path = unsafe { slice::from_raw_parts(path, path_len) };
    let out = unsafe { slice::from_raw_parts_mut(out, cap) };
    if out.len() < 2 {
        return 0;
    }
    out[0] = b'/';
    let mut len = 1;
    if !path.starts_with(b"/") {
        let Some(n) = append_components(base, out, len) else {
            return 0;
        };
        len = n;
    }
    let Some(n) = append_components(path, out, len) else {
        return 0;
    };
    let Some(terminator) = out.get_mut(n) else {
        return 0;
    };
    *terminator = 0;
    n
}
