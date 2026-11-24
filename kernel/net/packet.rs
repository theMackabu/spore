#[repr(C)]
#[derive(Default)]
pub struct RxHeader {
    eth_type: u16,
    ip_protocol: u8,
    ttl: u8,
    ip_header_len: u16,
    transport_len: u16,
    src_ip: u32,
    dst_ip: u32,
    src_port: u16,
    dst_port: u16,
    sequence: u32,
    acknowledgement: u32,
    window: u16,
    tcp_flags: u8,
    tcp_options_len: u8,
    arp_operation: u16,
    icmp_type: u8,
    quoted_protocol: u8,
    quoted_src: u32,
    quoted_dst: u32,
}

fn be16(data: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([data[at], data[at + 1]])
}
fn octets(data: &[u8], at: usize) -> [u8; 4] {
    [data[at], data[at + 1], data[at + 2], data[at + 3]]
}
fn sum_bytes(data: &[u8]) -> u32 {
    let (pairs, tail) = data.as_chunks::<2>();
    let mut sum = 0;
    for pair in pairs {
        sum += u32::from(u16::from_be_bytes(*pair));
    }
    if let [last] = tail {
        sum += u32::from(*last) << 8;
    }
    sum
}

fn fold(mut sum: u64) -> u16 {
    while sum > 0xffff {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum as u16
}

fn checksum(data: &[u8], initial: u64) -> u16 {
    if data.len() <= 32 {
        return !fold(initial + u64::from(sum_bytes(data)));
    }
    let (prefix, words, suffix) = unsafe { data.align_to::<u32>() };
    let native_sum: u64 = words.iter().map(|&word| u64::from(word)).sum();
    let middle = u16::from_be(fold(native_sum));
    let mut rest = fold(u64::from(middle) + u64::from(sum_bytes(suffix)));
    if prefix.len() % 2 != 0 {
        rest = rest.swap_bytes();
    }
    !fold(initial + u64::from(sum_bytes(prefix)) + u64::from(rest))
}

fn transport_checksum(src: u32, dst: u32, protocol: u8, segment: &[u8]) -> u16 {
    let pseudo = u64::from(src.swap_bytes())
        + u64::from(dst.swap_bytes())
        + u64::from(protocol)
        + segment.len() as u64;
    checksum(segment, pseudo)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn net_checksum(data: *const u8, len: u16) -> u16 {
    checksum(
        unsafe { core::slice::from_raw_parts(data, usize::from(len)) },
        0,
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn net_transport_checksum(
    src: u32,
    dst: u32,
    protocol: u8,
    data: *const u8,
    len: u16,
) -> u16 {
    transport_checksum(src, dst, protocol, unsafe {
        core::slice::from_raw_parts(data, usize::from(len))
    })
}

fn ip_header(data: &[u8]) -> Option<&[u8]> {
    if data.len() < 20 || data[0] >> 4 != 4 {
        return None;
    }
    let header = data.get(..(data[0] & 15) as usize * 4)?;
    (header.len() >= 20 && checksum(header, 0) == 0).then_some(header)
}

fn decode_frame(frame: &[u8]) -> Option<RxHeader> {
    let data = frame.get(14..)?;
    let mut h = RxHeader {
        eth_type: be16(frame, 12),
        ..Default::default()
    };
    match h.eth_type {
        0x0806 => {
            if data.len() < 28 || data[..6] != [0, 1, 8, 0, 6, 4] {
                return None;
            }
            h.arp_operation = be16(data, 6);
            h.src_ip = u32::from_le_bytes(octets(data, 14));
            h.dst_ip = u32::from_le_bytes(octets(data, 24));
            return Some(h);
        }
        0x0800 => (),
        _ => return None,
    }
    let ip = ip_header(data)?;
    let segment = data.get(ip.len()..be16(ip, 2) as usize)?;
    if ip[8] == 0 || be16(ip, 6) & 0xbfff != 0 {
        return None;
    }
    h.ip_protocol = ip[9];
    h.ttl = ip[8];
    h.ip_header_len = ip.len() as u16;
    h.transport_len = segment.len() as u16;
    h.src_ip = u32::from_le_bytes(octets(ip, 12));
    h.dst_ip = u32::from_le_bytes(octets(ip, 16));
    match h.ip_protocol {
        1 => {
            if segment.len() < 8 || checksum(segment, 0) != 0 {
                return None;
            }
            h.icmp_type = segment[0];
            if segment[..2] == [3, 3] {
                let quote = &segment[8..];
                let nested = ip_header(quote)?;
                let ports = quote.get(nested.len()..nested.len() + 8)?;
                h.quoted_protocol = nested[9];
                h.quoted_src = u32::from_le_bytes(octets(nested, 12));
                h.quoted_dst = u32::from_le_bytes(octets(nested, 16));
                h.src_port = be16(ports, 0);
                h.dst_port = be16(ports, 2);
            }
        }
        6 => {
            if segment.len() < 20
                || transport_checksum(h.src_ip, h.dst_ip, h.ip_protocol, segment) != 0
            {
                return None;
            }
            let header_len = (segment[12] >> 4) as usize * 4;
            if !(20..=segment.len()).contains(&header_len) {
                return None;
            }
            let flags = segment[13];
            if (flags & 7).count_ones() > 1 {
                return None;
            }
            h.src_port = be16(segment, 0);
            h.dst_port = be16(segment, 2);
            if h.src_port == 0 || h.dst_port == 0 {
                return None;
            }
            h.sequence = u32::from_be_bytes(octets(segment, 4));
            h.acknowledgement = u32::from_be_bytes(octets(segment, 8));
            h.window = be16(segment, 14);
            h.tcp_flags = flags;
            h.tcp_options_len = (header_len - 20) as u8;
        }
        17 => {
            if segment.len() < 8 {
                return None;
            }
            let udp = segment.get(..be16(segment, 4) as usize)?;
            if udp.len() < 8
                || (be16(udp, 6) != 0
                    && transport_checksum(h.src_ip, h.dst_ip, h.ip_protocol, udp) != 0)
            {
                return None;
            }
            h.src_port = be16(udp, 0);
            h.dst_port = be16(udp, 2);
            h.transport_len = udp.len() as u16;
            if h.dst_port == 0 {
                return None;
            }
        }
        _ => return None,
    }
    Some(h)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_net_decode_ethernet(
    frame: *const u8,
    len: usize,
    out: *mut RxHeader,
) -> bool {
    if frame.is_null() || out.is_null() {
        return false;
    }
    let Some(header) = decode_frame(unsafe { core::slice::from_raw_parts(frame, len) }) else {
        return false;
    };
    unsafe { out.write(header) };
    true
}

#[repr(C)]
pub struct TcpOptions {
    mss: u16,
    window_scale: u8,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_net_decode_tcp_options(data: *const u8, len: usize) -> TcpOptions {
    let mut options = if data.is_null() {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(data, len) }
    };
    let (mut mss, mut scale) = (None, None);
    while let Some((&kind, tail)) = options.split_first() {
        if kind == 0 {
            break;
        }
        if kind == 1 {
            options = tail;
            continue;
        }
        let Some(&len) = tail.first() else { break };
        let Some(value) = options.get(2..len as usize) else {
            break;
        };
        match (kind, value) {
            (2, [a, b]) if mss.is_none() => mss = Some(u16::from_be_bytes([*a, *b]).max(536)),
            (3, [s]) if scale.is_none() => scale = Some((*s).min(14)),
            _ => (),
        }
        options = &options[len as usize..];
    }
    TcpOptions {
        mss: mss.unwrap_or(1460),
        window_scale: scale.unwrap_or(0),
    }
}
