use std::env;
use std::fs;
use std::net::{Ipv4Addr, SocketAddrV4, UdpSocket};
use std::process::ExitCode;
use std::time::Duration;

#[repr(C)]
struct NetConfig { local_ip: u32, gateway_ip: u32, netmask: u32, dns_ip: u32, configured: u32 }

unsafe extern "C" {
    fn net_config_get(out: *mut NetConfig) -> bool;
    fn net_config_set(config: *const NetConfig) -> bool;
}

fn ip(value: u32) -> Ipv4Addr { Ipv4Addr::from(value.to_le_bytes()) }
fn be32(value: u32) -> [u8; 4] { value.to_be_bytes() }
fn le_ip(bytes: &[u8]) -> u32 { u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) }

fn packet(kind: u8, xid: u32, requested: u32, server: u32) -> Vec<u8> {
    let mut out = vec![0u8; 240];
    out[0..4].copy_from_slice(&[1, 1, 6, 0]);
    out[4..8].copy_from_slice(&be32(xid));
    out[10] = 0x80;
    out[28..34].copy_from_slice(&[0x52, 0x54, 0, 0x12, 0x34, 0x56]);
    out[236..240].copy_from_slice(&be32(0x63825363));
    out.extend_from_slice(&[53, 1, kind]);
    if requested != 0 { out.extend_from_slice(&[50, 4]); out.extend_from_slice(&requested.to_le_bytes()); }
    if server != 0 { out.extend_from_slice(&[54, 4]); out.extend_from_slice(&server.to_le_bytes()); }
    out.extend_from_slice(&[55, 3, 1, 3, 6, 255]);
    out
}

#[derive(Default)]
struct Lease { kind: u8, address: u32, server: u32, netmask: u32, router: u32, dns: u32 }

fn parse(packet: &[u8], xid: u32) -> Option<Lease> {
    if packet.len() < 240 || packet[0] != 2 || packet[4..8] != be32(xid) { return None; }
    let mut lease = Lease { address: le_ip(&packet[16..20]), ..Lease::default() };
    let mut index = 240;
    while index < packet.len() {
        let option = packet[index]; index += 1;
        if option == 255 { break; }
        if option == 0 { continue; }
        if index >= packet.len() { break; }
        let size = packet[index] as usize; index += 1;
        if index + size > packet.len() { break; }
        let data = &packet[index..index + size];
        match option {
            53 if size >= 1 => lease.kind = data[0],
            54 | 1 | 3 | 6 if size >= 4 => match option {
                54 => lease.server = le_ip(data), 1 => lease.netmask = le_ip(data),
                3 => lease.router = le_ip(data), _ => lease.dns = le_ip(data),
            },
            _ => {},
        }
        index += size;
    }
    (lease.kind != 0 && lease.address != 0).then_some(lease)
}

fn receive(socket: &UdpSocket, xid: u32, kind: u8) -> Option<Lease> {
    socket.set_read_timeout(Some(Duration::from_millis(2500))).ok()?;
    let mut bytes = [0u8; 600];
    loop {
        let (size, _) = socket.recv_from(&mut bytes).ok()?;
        if let Some(lease) = parse(&bytes[..size], xid) { if lease.kind == kind { return Some(lease); } }
    }
}

fn main() -> ExitCode {
    if env::args().nth(1).as_deref() == Some("--address") {
        let mut config = NetConfig { local_ip: 0, gateway_ip: 0, netmask: 0, dns_ip: 0, configured: 0 };
        return if unsafe { net_config_get(&mut config) } && config.local_ip != 0 { println!("{}", ip(config.local_ip)); ExitCode::SUCCESS } else { ExitCode::FAILURE };
    }
    let socket = match UdpSocket::bind(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 68)) { Ok(socket) => socket, Err(error) => { eprintln!("dhcpc: {error}"); return ExitCode::FAILURE; } };
    if socket.set_broadcast(true).is_err() { return ExitCode::FAILURE; }
    let xid = 0x53504f52;
    let destination = SocketAddrV4::new(Ipv4Addr::BROADCAST, 67);
    if socket.send_to(&packet(1, xid, 0, 0), destination).is_err() { return ExitCode::FAILURE; }
    let offer = match receive(&socket, xid, 2) { Some(lease) => lease, None => { eprintln!("dhcpc: no offer"); return ExitCode::FAILURE; } };
    if socket.send_to(&packet(3, xid, offer.address, offer.server), destination).is_err() { return ExitCode::FAILURE; }
    let mut ack = match receive(&socket, xid, 5) { Some(lease) => lease, None => { eprintln!("dhcpc: no ack"); return ExitCode::FAILURE; } };
    if ack.router == 0 { ack.router = offer.server; }
    if ack.dns == 0 { ack.dns = offer.server; }
    if ack.netmask == 0 { ack.netmask = 0x00ff_ffff; }
    let config = NetConfig { local_ip: ack.address, gateway_ip: ack.router, netmask: ack.netmask, dns_ip: ack.dns, configured: 1 };
    if !unsafe { net_config_set(&config) } { return ExitCode::FAILURE; }
    let _ = fs::write("/etc/resolv.conf", format!("nameserver {}\n", ip(ack.dns)));
    println!("leased {} gateway {} dns {}", ip(config.local_ip), ip(config.gateway_ip), ip(config.dns_ip));
    ExitCode::SUCCESS
}
