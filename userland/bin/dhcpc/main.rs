#![no_std]

#[path = "../../lib/spore/panic.rs"]
mod panic;
mod socket;

use core::ffi::{CStr, c_char, c_int, c_void};
use socket::Socket;

const COOKIE: u32 = 0x63825363;
const XID: u32 = 0x53504f52;
const CLIENT_MAC: [u8; 6] = [0x52, 0x54, 0, 0x12, 0x34, 0x56];

#[repr(C)]
#[derive(Default)]
struct NetConfig {
    local_ip: u32,
    gateway_ip: u32,
    netmask: u32,
    dns_ip: u32,
    configured: u32,
}

unsafe extern "C" {
    fn net_config_get(config: &mut NetConfig) -> bool;
    fn net_config_set(config: &NetConfig) -> bool;
    fn format_ipv4(ip: u32, buf: *mut c_char, capacity: usize);
    fn dprintf(fd: c_int, format: *const c_char, ...) -> c_int;
    fn fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
    fn fprintf(file: *mut c_void, format: *const c_char, ...) -> c_int;
    fn fclose(file: *mut c_void) -> c_int;
}

fn address_text(ip: u32) -> [c_char; 16] {
    let mut buf = [0; 16];
    unsafe {
        format_ipv4(ip, buf.as_mut_ptr(), buf.len());
    }
    buf
}

fn le_ip(bytes: &[u8]) -> u32 {
    u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

struct Packet {
    bytes: [u8; 261],
    len: usize,
}

impl Packet {
    fn new(kind: u8, requested: u32, server: u32) -> Self {
        let mut packet = Self {
            bytes: [0; 261],
            len: 240,
        };
        packet.bytes[..3].copy_from_slice(&[1, 1, 6]);
        packet.bytes[4..8].copy_from_slice(&XID.to_be_bytes());
        packet.bytes[10] = 0x80;
        packet.bytes[28..34].copy_from_slice(&CLIENT_MAC);
        packet.bytes[236..240].copy_from_slice(&COOKIE.to_be_bytes());
        packet.append(&[53, 1, kind]);
        for (option, value) in [(50, requested), (54, server)] {
            if value != 0 {
                packet.append(&[option, 4]);
                packet.append(&value.to_le_bytes());
            }
        }
        packet.append(&[55, 3, 1, 3, 6, 255]);
        packet
    }

    fn append(&mut self, data: &[u8]) {
        self.bytes[self.len..self.len + data.len()].copy_from_slice(data);
        self.len += data.len();
    }

    fn as_slice(&self) -> &[u8] {
        &self.bytes[..self.len]
    }
}

#[derive(Default)]
struct Lease {
    kind: u8,
    address: u32,
    server: u32,
    netmask: u32,
    router: u32,
    dns: u32,
}

fn parse(packet: &[u8], xid: u32) -> Option<Lease> {
    if packet.len() < 240 || packet[0] != 2 || packet[4..8] != xid.to_be_bytes() {
        return None;
    }
    let mut lease = Lease {
        address: le_ip(&packet[16..20]),
        ..Lease::default()
    };
    let mut index = 240;
    while index < packet.len() {
        let option = packet[index];
        index += 1;
        if option == 255 {
            break;
        }
        if option == 0 {
            continue;
        }
        if index >= packet.len() {
            break;
        }
        let size = packet[index] as usize;
        index += 1;
        if index + size > packet.len() {
            break;
        }
        let data = &packet[index..index + size];
        match option {
            53 if size >= 1 => lease.kind = data[0],
            54 | 1 | 3 | 6 if size >= 4 => match option {
                54 => lease.server = le_ip(data),
                1 => lease.netmask = le_ip(data),
                3 => lease.router = le_ip(data),
                _ => lease.dns = le_ip(data),
            },
            _ => {}
        }
        index += size;
    }
    (lease.kind != 0 && lease.address != 0).then_some(lease)
}

fn exchange(socket: &Socket, packet: Packet, kind: u8) -> Option<Lease> {
    if !socket.broadcast(67, packet.as_slice()) {
        return None;
    }
    let mut bytes = [0; 600];
    loop {
        let data = socket.receive(&mut bytes, 2500)?;
        if let Some(lease) = parse(data, XID)
            && lease.kind == kind
        {
            return Some(lease);
        }
    }
}

fn configure() -> Result<(), &'static CStr> {
    let socket = Socket::bind_broadcast(68).ok_or(c"cannot open DHCP socket")?;
    let offer = exchange(&socket, Packet::new(1, 0, 0), 2).ok_or(c"no offer")?;
    let ack = exchange(&socket, Packet::new(3, offer.address, offer.server), 5).ok_or(c"no ack")?;
    let config = NetConfig {
        local_ip: ack.address,
        gateway_ip: if ack.router == 0 {
            offer.server
        } else {
            ack.router
        },
        netmask: if ack.netmask == 0 {
            0x00ff_ffff
        } else {
            ack.netmask
        },
        dns_ip: if ack.dns == 0 { offer.server } else { ack.dns },
        configured: 1,
    };
    if !unsafe { net_config_set(&config) } {
        return Err(c"cannot set network configuration");
    }
    let dns = address_text(config.dns_ip);
    unsafe {
        let file = fopen(c"/etc/resolv.conf".as_ptr(), c"w".as_ptr());
        if !file.is_null() {
            fprintf(file, c"nameserver %s\n".as_ptr(), dns.as_ptr());
            fclose(file);
        }
        dprintf(
            1,
            c"leased %s gateway %s dns %s\n".as_ptr(),
            address_text(config.local_ip).as_ptr(),
            address_text(config.gateway_ip).as_ptr(),
            dns.as_ptr(),
        );
    }
    Ok(())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn main(argc: c_int, argv: *const *const c_char) -> c_int {
    if argc == 2 && unsafe { CStr::from_ptr(*argv.add(1)) } == c"--address" {
        let mut config = NetConfig::default();
        if !unsafe { net_config_get(&mut config) } || config.local_ip == 0 {
            return 1;
        }
        unsafe {
            dprintf(1, c"%s\n".as_ptr(), address_text(config.local_ip).as_ptr());
        }
        return 0;
    }
    if argc != 1 {
        unsafe {
            dprintf(2, c"usage: dhcpc [--address]\n".as_ptr());
        }
        return 64;
    }
    match configure() {
        Ok(()) => 0,
        Err(message) => {
            unsafe {
                dprintf(2, c"dhcpc: %s\n".as_ptr(), message.as_ptr());
            }
            1
        }
    }
}
