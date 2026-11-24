#![no_std]

use core::panic::PanicInfo;

#[repr(C)]
pub struct NetTcpOptions {
    pub mss: u16,
    pub window_scale: u8,
    pub has_mss: u8,
}

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {}
}

fn be16(bytes: &[u8]) -> u16 {
    u16::from_be_bytes([bytes[0], bytes[1]])
}

fn checksum(bytes: &[u8]) -> u16 {
    let mut sum = 0u32;
    let mut chunks = bytes.chunks_exact(2);
    for word in &mut chunks {
        sum += be16(word) as u32;
    }
    if let Some(&byte) = chunks.remainder().first() {
        sum += (byte as u32) << 8;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

#[no_mangle]
pub unsafe extern "C" fn spore_net_validate_ethernet(frame: *const u8, len: usize) -> bool {
    if frame.is_null() || len < 14 {
        return false;
    }
    let frame = core::slice::from_raw_parts(frame, len);
    match be16(&frame[12..14]) {
        0x0806 => frame.len() >= 42 && be16(&frame[14..16]) == 1 && be16(&frame[16..18]) == 0x0800 &&
                  frame[18] == 6 && frame[19] == 4,
        0x0800 => {
            let ip = &frame[14..];
            if ip.len() < 20 || ip[0] >> 4 != 4 {
                return false;
            }
            let ihl = ((ip[0] & 15) as usize) * 4;
            if ihl < 20 || ihl > ip.len() || checksum(&ip[..ihl]) != 0 {
                return false;
            }
            let total = be16(&ip[2..4]) as usize;
            if total < ihl || total > ip.len() || ip[8] == 0 || be16(&ip[6..8]) & 0xbfff != 0 {
                return false;
            }
            let payload = &ip[ihl..total];
            match ip[9] {
                1 => payload.len() >= 8 && checksum(payload) == 0,
                6 => {
                    if payload.len() < 20 || payload[0] == 0 || payload[1] == 0 {
                        return false;
                    }
                    let offset = ((payload[12] >> 4) as usize) * 4;
                    offset >= 20 && offset <= payload.len() &&
                        (payload[13] & 3 != 3) && !(payload[13] & 2 != 0 && payload[13] & 5 != 0)
                }
                17 => payload.len() >= 8 && payload[0] != 0 && payload[1] != 0 &&
                      (be16(&payload[4..6]) as usize) >= 8 && (be16(&payload[4..6]) as usize) <= payload.len(),
                _ => true,
            }
        }
        _ => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn spore_net_decode_tcp_options(
    options: *const u8,
    len: usize,
    out: *mut NetTcpOptions,
) -> bool {
    if out.is_null() || (options.is_null() && len != 0) || len > 40 {
        return false;
    }
    let options = core::slice::from_raw_parts(options, len);
    let mut decoded = NetTcpOptions { mss: 1460, window_scale: 0, has_mss: 0 };
    let mut index = 0;
    while index < options.len() {
        match options[index] {
            0 => break,
            1 => index += 1,
            kind => {
                if index + 1 >= options.len() {
                    return false;
                }
                let size = options[index + 1] as usize;
                if size < 2 || index + size > options.len() {
                    return false;
                }
                if kind == 2 && size == 4 {
                    decoded.mss = be16(&options[index + 2..index + 4]);
                    decoded.has_mss = 1;
                } else if kind == 3 && size == 3 {
                    decoded.window_scale = options[index + 2].min(14);
                }
                index += size;
            }
        }
    }
    *out = decoded;
    true
}
