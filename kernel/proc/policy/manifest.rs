use core::ffi::c_char;
use core::ptr;

#[repr(u32)]
enum Profile {
    Compute,
    Files,
    Memory,
    Network,
}

#[repr(C)]
#[derive(Default)]
struct Network {
    address: u32,
    mask: u32,
    port: u16,
    protocol: u8,
}

#[repr(C)]
pub struct Request {
    profile: Profile,
    network: Network,
    executable: *const c_char,
}

fn number(text: &[u8]) -> Option<u16> {
    if text.is_empty() {
        return None;
    }
    text.iter().try_fold(0u16, |n, &c| {
        let digit = c.checked_sub(b'0')?;
        if digit > 9 {
            return None;
        }
        n.checked_mul(10)?.checked_add(u16::from(digit))
    })
}

fn endpoint(text: &[u8], protocol: u8) -> Option<Network> {
    let colon = text.iter().position(|&c| c == b':')?;
    let mut host = &text[..colon];
    let port = number(&text[colon + 1..])?;
    let mut mask = u32::MAX;
    if let Some(slash) = host.iter().position(|&c| c == b'/') {
        let prefix = number(&host[slash + 1..])?;
        if prefix > 32 {
            return None;
        }
        mask = u32::MAX.checked_shl(u32::from(32 - prefix)).unwrap_or(0);
        host = &host[..slash];
    }
    let mut octets = host.split(|&c| c == b'.');
    let mut ip = [0; 4];
    for octet in &mut ip {
        *octet = u8::try_from(number(octets.next()?)?).ok()?;
    }
    if octets.next().is_some() {
        return None;
    }
    Some(Network {
        address: (u32::from_be_bytes(ip) & mask).swap_bytes(),
        mask: mask.swap_bytes(),
        protocol,
        port,
    })
}

fn parse(text: &[u8]) -> Option<Request> {
    let mut network = Network::default();
    let mut executable = ptr::null();
    let profile = match text {
        b"compute-only" => Profile::Compute,
        b"mem:1" => Profile::Memory,
        b"fs:/tmp" => Profile::Files,
        b"net:none" => Profile::Network,
        b"net:dns" => {
            network.protocol = 17;
            network.port = 53;
            Profile::Network
        }
        _ => {
            if let Some(path) = text.strip_prefix(b"fs:/tmp;exec:") {
                if !path.starts_with(b"/") {
                    return None;
                }
                executable = path.as_ptr().cast();
                Profile::Files
            } else {
                network = if let Some(target) = text.strip_prefix(b"net:udp:") {
                    endpoint(target, 17)?
                } else {
                    endpoint(text.strip_prefix(b"net:tcp:")?, 6)?
                };
                Profile::Network
            }
        }
    };
    Some(Request {
        profile,
        network,
        executable,
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn spore_parse_manifest(text: *const c_char, out: *mut Request) -> bool {
    let manifest = unsafe { core::ffi::CStr::from_ptr(text) }.to_bytes();
    let Some(request) = parse(manifest) else {
        return false;
    };
    unsafe { out.write(request) };
    true
}
