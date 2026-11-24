use std::{
    ffi::OsString,
    fs::{self, File},
    io::{self, BufRead, BufReader, Read, Write},
    os::fd::AsFd,
    path::Path,
    process::{Child, ChildStdout, Command, Stdio},
    thread,
    time::{Duration, Instant},
};
unsafe extern "C" {
    fn runner_terminal_open(interactive: bool);
    fn runner_interrupted() -> i32;
    fn runner_terminal_close();
}
struct Vm(Child);
impl Drop for Vm {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
        unsafe { runner_terminal_close() };
    }
}

fn relay_console(serial: ChildStdout) -> io::Result<()> {
    let mut serial = BufReader::new(serial);
    let mut line = Vec::new();
    let mut diagnostic = Vec::new();
    let marker = b"[kernel] Limine handoff at EL1,";
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

pub fn run(
    q: &[OsString],
    control: &Path,
    result_path: Option<&Path>,
    deadline: u64,
) -> io::Result<i32> {
    let mut status = File::create_new(control).and_then(|_| File::open(control))?;
    let start = Instant::now();
    let mut vm = Vm(Command::new(&q[0])
        .args(&q[1..])
        .stdout(Stdio::piped())
        .spawn()?);
    let serial = vm.0.stdout.take().unwrap();
    let output = thread::spawn(move || relay_console(serial));
    unsafe { runner_terminal_open(false) };
    let mut entry = None;
    let mut bytes = Vec::new();
    let mut protocol = false;
    let (kind, value) = loop {
        let exited = vm.0.try_wait()?;
        status.read_to_end(&mut bytes)?;
        let magic = b"\0SPORE1\0";
        if !protocol {
            if let Some(i) = bytes.windows(magic.len()).position(|w| w == magic) {
                bytes.drain(..i + magic.len());
                protocol = true;
            } else if bytes.len() >= magic.len() {
                bytes.drain(..bytes.len() - (magic.len() - 1));
            }
        }
        let records = if protocol { bytes.as_slice() } else { &[] };
        if entry.is_none() && records.starts_with(b"U\0") {
            entry = Some(start.elapsed().as_micros());
        }
        if let Some(status) = exited {
            if !status.success() {
                break ("vm-error", 125);
            }
            break match records {
                [b'U', 0, b'E', n] => ("exit", i32::from(*n)),
                [b'U', 0, b'S', n] => ("signal", i32::from(*n)),
                [b'B', n] | [b'U', 0, b'B', n] => ("boot-error", i32::from(*n)),
                _ => ("vm-error", 125),
            };
        }
        let sig = unsafe { runner_interrupted() };
        if sig != 0 {
            break ("host-signal", sig);
        }
        if entry.is_none() && start.elapsed() >= Duration::from_secs(deadline) {
            break ("boot-timeout", 125);
        }
        thread::sleep(Duration::from_millis(if entry.is_some() { 20 } else { 2 }));
    };
    drop(vm);
    output
        .join()
        .map_err(|_| io::Error::other("console relay failed"))??;
    report(result_path, kind, value, entry)?;
    eprintln!("spore: {kind} {value}");
    Ok(match kind {
        "exit" => value,
        "signal" | "host-signal" => 128 + value,
        _ => 125,
    })
}
