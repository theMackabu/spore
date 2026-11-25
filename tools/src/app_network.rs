use std::{io, net::Ipv4Addr};

pub fn user_network([guest, gateway, mask, dns]: [Ipv4Addr; 4]) -> io::Result<String> {
    let mask = u32::from(mask);
    let prefix = mask.leading_ones();
    let base = u32::from(guest) & mask;
    let last = base | !mask;
    if !(1..=28).contains(&prefix)
        || mask.count_ones() != prefix
        || [guest, gateway]
            .iter()
            .any(|ip| !(base + 1..last).contains(&u32::from(*ip)))
        || guest == gateway
        || guest == dns
        || gateway == dns
    {
        return Err(io::Error::other(
            "invalid guest IPv4 configuration for QEMU user networking",
        ));
    }
    let mut cfg = format!(
        "user,id=sporenet,net={}/{prefix},host={gateway}",
        Ipv4Addr::from(base)
    );
    if u32::from(dns) & mask == base {
        cfg.push_str(&format!(",dns={dns}"));
    }
    Ok(cfg)
}

pub fn forwards(mut backend: String, guest: &str, ports: &[(u16, u16)]) -> String {
    for (host, port) in ports {
        backend.push_str(&format!(",hostfwd=tcp:127.0.0.1:{host}-{guest}:{port}"));
    }
    backend
}
