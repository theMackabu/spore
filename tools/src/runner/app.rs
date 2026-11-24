use std::{
    ffi::OsString,
    fs::{self, File},
    io::{self, BufRead, BufReader, Read, Write},
    os::fd::{AsFd, AsRawFd},
    os::unix::{net::UnixStream, process::CommandExt},
    path::Path,
    process::{Child, ChildStdout, Command, Stdio},
    thread,
    time::{Duration, Instant},
};
unsafe extern "C" {
    fn runner_terminal_open(interactive: bool, events: i32);
    fn runner_poll(status: i32, events: i32, timeout: i32) -> i32;
    fn runner_inherit_fd(fd: i32) -> i32;
    fn runner_interrupted() -> i32;
    fn runner_terminal_close();
}
struct Terminal;
impl Terminal {
    fn open(events: &UnixStream) -> Self {
        unsafe { runner_terminal_open(true, events.as_raw_fd()) };
        Self
    }
}
impl Drop for Terminal {
    fn drop(&mut self) {
        unsafe { runner_terminal_close() };
    }
}

struct Vm(Child);
impl Drop for Vm {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn relay_console(serial: ChildStdout, marker: &[u8]) -> io::Result<()> {
    let mut serial = BufReader::new(serial);
    let mut line = Vec::new();
    let mut diagnostic = Vec::new();
    loop {
        line.clear();
        if serial.read_until(b'\n', &mut line)? == 0 {
            return io::stderr().write_all(&diagnostic);
        }
        if line.windows(marker.len()).any(|w| w == marker) {
            let mut output = File::from(io::stdout().as_fd().try_clone_to_owned()?);
            io::copy(&mut serial, &mut output)?;
            return Ok(());
        }
        diagnostic.extend_from_slice(&line);
        if diagnostic.len() > 4096 {
            diagnostic.drain(..diagnostic.len() - 4096);
        }
    }
}

pub fn report(path: Option<&Path>, kind: &str, value: i32, entry: Option<u128>) -> io::Result<()> {
    if let Some(path) = path {
        fs::write(
            path,
            format!(
                "{{\"kind\":\"{kind}\",\"value\":{value},\"entry_us\":{}}}\n",
                entry.map_or_else(|| "null".into(), |n| n.to_string())
            ),
        )?;
    }
    Ok(())
}

#[derive(Default)]
struct GuestStatus {
    prefix: usize,
    records: Vec<u8>,
    entry: Option<u128>,
}

impl GuestStatus {
    fn receive(&mut self, bytes: &[u8], start: Instant) {
        const MAGIC: &[u8] = b"\0SPORE1\0";
        for &byte in bytes {
            if self.prefix < MAGIC.len() {
                self.prefix = if byte == MAGIC[self.prefix] {
                    self.prefix + 1
                } else {
                    usize::from(byte == MAGIC[0])
                };
            } else if self.records.len() < 5 {
                self.records.push(byte);
                if self.records == b"U\0" {
                    self.entry = Some(start.elapsed().as_micros());
                }
            }
        }
    }

    fn result(&self) -> (&'static str, i32) {
        match self.records.as_slice() {
            [b'U', 0, b'E', n] => ("exit", i32::from(*n)),
            [b'U', 0, b'S', n] => ("signal", i32::from(*n)),
            [b'B', n] | [b'U', 0, b'B', n] => ("boot-error", i32::from(*n)),
            _ => ("vm-error", 125),
        }
    }
}

pub enum StatusDevice {
    Virtio,
    Serial,
}

pub fn run(
    mut q: Vec<OsString>,
    result_path: Option<&Path>,
    deadline: u64,
    device: StatusDevice,
    marker: &'static [u8],
) -> io::Result<i32> {
    let (mut status, peer) = UnixStream::pair()?;
    let (mut events, notify) = UnixStream::pair()?;
    notify.set_nonblocking(true)?;
    status.set_nonblocking(true)?;
    let _terminal = Terminal::open(&notify);
    let fd = peer.as_raw_fd();
    q.extend([
        "-no-reboot".into(),
        "-chardev".into(),
        format!("socket,id=status,fd={fd},server=off").into(),
    ]);
    match device {
        StatusDevice::Serial => q.extend(["-serial".into(), "chardev:status".into()]),
        StatusDevice::Virtio => q.extend([
            "-device".into(),
            "virtio-serial-device,id=statusbus".into(),
            "-device".into(),
            "virtconsole,bus=statusbus.0,chardev=status".into(),
        ]),
    }
    let mut cmd = Command::new(&q[0]);
    cmd.args(&q[1..]).stdout(Stdio::piped());
    unsafe {
        cmd.pre_exec(move || {
            if runner_inherit_fd(fd) == -1 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let start = Instant::now();
    let mut vm = Vm(cmd.spawn()?);
    drop(peer);
    let serial = vm.0.stdout.take().unwrap();
    let output = thread::spawn(move || relay_console(serial, marker));
    let mut guest = GuestStatus::default();
    let mut open = true;
    let mut buffer = [0; 512];
    let (kind, value) = loop {
        let exited = vm.0.try_wait()?;
        while open {
            match status.read(&mut buffer) {
                Ok(0) => open = false,
                Ok(n) => {
                    guest.receive(&buffer[..n], start);
                    if exited.is_none() {
                        break;
                    }
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) if e.kind() == io::ErrorKind::ConnectionReset => open = false,
                Err(e) => return Err(e),
            }
        }
        let sig = unsafe { runner_interrupted() };
        if sig != 0 {
            break ("host-signal", sig);
        }
        if let Some(status) = exited {
            break if status.success() {
                guest.result()
            } else {
                ("vm-error", 125)
            };
        }
        let timeout = if guest.entry.is_some() {
            -1
        } else {
            let left = Duration::from_secs(deadline).saturating_sub(start.elapsed());
            if left.is_zero() {
                break ("boot-timeout", 125);
            }
            left.as_millis().clamp(1, i32::MAX as u128) as i32
        };
        let ready = unsafe {
            runner_poll(
                if open { status.as_raw_fd() } else { -1 },
                events.as_raw_fd(),
                timeout,
            )
        };
        if ready < 0 {
            return Err(io::Error::last_os_error());
        }
        if ready & 2 != 0 {
            events.read_exact(&mut buffer[..1])?;
        }
    };
    drop(vm);
    output
        .join()
        .map_err(|_| io::Error::other("console relay failed"))??;
    report(result_path, kind, value, guest.entry)?;
    eprintln!("spore: {kind} {value}");
    Ok(match kind {
        "exit" => value,
        "signal" | "host-signal" => 128 + value,
        _ => 125,
    })
}
