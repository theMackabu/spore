#![no_std]

#[path = "../../lib/spore/panic.rs"]
mod panic;
mod socket;

use core::ffi::{CStr, c_char, c_int, c_void};
use socket::Socket;

const COOKIE: u32 = 0x63825363;
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
    fn getentropy(buf: *mut c_void, len: usize) -> c_int;
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
    fn new(xid: u32, kind: u8, requested: u32, server: u32) -> Self {
        let mut packet = Self {
            bytes: [0; 261],
            len: 240,
        };
        packet.bytes[..3].copy_from_slice(&[1, 1, 6]);
        packet.bytes[4..8].copy_from_slice(&xid.to_be_bytes());
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
    let mut options = packet.get(240..)?;
    if packet[..3] != [2, 1, 6]
        || packet[4..8] != xid.to_be_bytes()
        || packet[28..34] != CLIENT_MAC
        || packet[236..240] != COOKIE.to_be_bytes()
    {
        return None;
    }
    let mut lease = Lease {
        address: le_ip(&packet[16..20]),
        ..Lease::default()
    };
    loop {
        let (&option, rest) = options.split_first()?;
        match option {
            0 => {
                options = rest;
                continue;
            }
            255 => break,
            _ => (),
        }
        let (&size, rest) = rest.split_first()?;
        let (data, next) = rest.split_at_checked(usize::from(size))?;
        match (option, data) {
            (53, [kind]) => lease.kind = *kind,
            (54, [_, _, _, _]) => lease.server = le_ip(data),
            (1, [_, _, _, _]) => lease.netmask = le_ip(data),
            (3 | 6, _) if size >= 4 && size % 4 == 0 => {
                if option == 3 {
                    lease.router = le_ip(data);
                } else {
                    lease.dns = le_ip(data);
                }
            }
            (1 | 3 | 6 | 53 | 54, _) => return None,
            _ => (),
        }
        options = next;
    }
    (lease.kind != 0 && lease.address != 0 && lease.server != 0).then_some(lease)
}

fn exchange(socket: &Socket, xid: u32, offer: Option<&Lease>) -> Option<Lease> {
    let (message, reply, request, server) = match offer {
        Some(lease) => (3, 5, lease.address, lease.server),
        None => (1, 2, 0, 0),
    };
    let packet = Packet::new(xid, message, request, server);
    if !socket.broadcast(67, packet.as_slice()) {
        return None;
    }
    let mut bytes = [0; 600];
    let deadline = socket::monotonic_ms()?.checked_add(2500)?;
    loop {
        let remaining = i32::try_from(deadline - socket::monotonic_ms()?).ok()?;
        if remaining <= 0 {
            return None;
        }
        let data = socket.receive(&mut bytes, remaining)?;
        if let Some(lease) = parse(data, xid)
            && lease.kind == reply
            && (server == 0 || lease.server == server)
            && (request == 0 || lease.address == request)
        {
            return Some(lease);
        }
    }
}

fn configure() -> Result<(), &'static CStr> {
    let socket = Socket::bind_broadcast(68).ok_or(c"cannot open DHCP socket")?;
    let mut xid = 0u32;
    if unsafe { getentropy((&raw mut xid).cast(), size_of::<u32>()) } != 0 {
        return Err(c"cannot create DHCP transaction");
    }
    let offer = exchange(&socket, xid, None).ok_or(c"no offer")?;
    let ack = exchange(&socket, xid, Some(&offer)).ok_or(c"no ack")?;
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
