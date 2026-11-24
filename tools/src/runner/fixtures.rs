use std::io::{self, Read, Write};
use std::net::{Ipv4Addr, TcpListener, UdpSocket};
use std::thread;

pub fn start_udp() -> io::Result<()> {
    let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 5555))?;
    thread::Builder::new()
        .name("udp-echo".into())
        .spawn(move || {
            let mut buf = [0u8; 2048];
            while let Ok((n, peer)) = socket.recv_from(&mut buf) {
                if n > 0 {
                    let _ = socket.send_to(&buf[..n], peer);
                }
            }
        })?;
    Ok(())
}

pub fn start_http() -> io::Result<()> {
    const RESPONSE: &[u8] = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 21\r\nConnection: close\r\n\r\nhello from host http\n";
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 8080))?;
    thread::Builder::new()
        .name("http-fixture".into())
        .spawn(move || {
            for mut client in listener.incoming().flatten() {
                let mut buf = [0u8; 1024];
                let _ = client.read(&mut buf);
                let _ = client.write_all(RESPONSE);
            }
        })?;
    Ok(())
}
