use core::ffi::c_int;
use core::mem::size_of_val;

const AF_INET: c_int = 2;
const SOCK_DGRAM: c_int = 2;
const SOL_SOCKET: c_int = 1;
const SO_BROADCAST: c_int = 6;
const POLLIN: i16 = 1;

#[repr(C)]
struct Address {
    family: u16,
    port: u16,
    ip: [u8; 4],
    padding: [u8; 8],
}

impl Address {
    fn new(ip: [u8; 4], port: u16) -> Self {
        Self {
            family: AF_INET as u16,
            port: port.to_be(),
            ip,
            padding: [0; 8],
        }
    }
}

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

unsafe extern "C" {
    fn socket(family: c_int, kind: c_int, protocol: c_int) -> c_int;
    fn bind(fd: c_int, address: &Address, size: u32) -> c_int;
    fn setsockopt(fd: c_int, level: c_int, option: c_int, value: &c_int, size: u32) -> c_int;
    fn sendto(
        fd: c_int,
        data: *const u8,
        size: usize,
        flags: c_int,
        address: &Address,
        addrlen: u32,
    ) -> isize;
    fn poll(fds: *mut PollFd, count: usize, timeout_ms: c_int) -> c_int;
    fn recv(fd: c_int, data: *mut u8, size: usize, flags: c_int) -> isize;
    fn close(fd: c_int) -> c_int;
}

pub struct Socket(c_int);

impl Socket {
    pub fn bind_broadcast(port: u16) -> Option<Self> {
        let fd = unsafe { socket(AF_INET, SOCK_DGRAM, 0) };
        if fd < 0 {
            return None;
        }
        let socket = Self(fd);
        let enabled: c_int = 1;
        let address = Address::new([0; 4], port);
        let ok = unsafe {
            setsockopt(
                fd,
                SOL_SOCKET,
                SO_BROADCAST,
                &enabled,
                size_of_val(&enabled) as u32,
            ) == 0
                && bind(fd, &address, size_of_val(&address) as u32) == 0
        };
        ok.then_some(socket)
    }

    pub fn broadcast(&self, port: u16, packet: &[u8]) -> bool {
        let dst = Address::new([255; 4], port);
        unsafe {
            sendto(
                self.0,
                packet.as_ptr(),
                packet.len(),
                0,
                &dst,
                size_of_val(&dst) as u32,
            ) == packet.len() as isize
        }
    }

    pub fn receive<'a>(&self, buf: &'a mut [u8], timeout_ms: c_int) -> Option<&'a [u8]> {
        let mut pfd = PollFd {
            fd: self.0,
            events: POLLIN,
            revents: 0,
        };
        if unsafe { poll(&mut pfd, 1, timeout_ms) } <= 0 {
            return None;
        }
        let size = unsafe { recv(self.0, buf.as_mut_ptr(), buf.len(), 0) };
        buf.get(..usize::try_from(size).ok()?)
    }
}

impl Drop for Socket {
    fn drop(&mut self) {
        unsafe {
            close(self.0);
        }
    }
}
