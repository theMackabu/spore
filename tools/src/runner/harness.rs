use super::modes;
use super::serial::SerialWindow;
use super::terminal_output::TerminalOutput;
use std::ffi::OsString;
use std::fs::File;
use std::io::{self, IsTerminal, Read, Write};
use std::os::fd::{AsFd, AsRawFd};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::time::{Duration, Instant};

unsafe extern "C" {
    fn runner_terminal_open(interactive: bool, events: i32);
    fn runner_terminal_close();
    fn runner_resized() -> bool;
    fn runner_interrupted() -> i32;
    fn runner_terminal_size(rows: *mut u16, cols: *mut u16);
    fn runner_poll(serial: i32, keyboard: i32, timeout: i32) -> i32;
}

fn reset_mouse() {
    if io::stdout().is_terminal() {
        let _ = io::stdout()
            .write_all(b"\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1015l\x1b[?2004l");
    }
}
struct Terminal;
impl Terminal {
    fn open(plain: bool) -> Self {
        reset_mouse();
        unsafe { runner_terminal_open(plain, -1) };
        Self
    }
    fn send_size(&self, writer: &mut impl Write) -> io::Result<()> {
        let (mut rows, mut cols) = (38, 96);
        unsafe {
            runner_terminal_size(&mut rows, &mut cols);
        }
        write!(writer, "\x1b]777;{rows};{cols}\x07")
    }
}
impl Drop for Terminal {
    fn drop(&mut self) {
        unsafe { runner_terminal_close() };
        reset_mouse();
    }
}

struct Vm(Child);
impl Drop for Vm {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

struct SerialInput(ChildStdin);
impl Write for SerialInput {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        let n = self.0.write(&data[..data.len().min(32)])?;
        if n < data.len() {
            std::thread::sleep(Duration::from_millis(1));
        }
        Ok(n)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.0.flush()
    }
}

const MARKERS: &[(&[u8], &str)] = &[
    (b"UEFI firmware", "edk2 first banner"),
    (b"BdsDxe: starting", "edk2 starts boot option"),
    (b"[kernel] Limine handoff", "Limine entered kernel"),
    (b"[kernel] booted", "kernel first log"),
    (b"[kernel] entering EL0", "enter EL0"),
    (b"$ ", "shell prompt"),
];

const INTEGRATION_CHECKS: &[(&[u8], &str)] = &[
    (
        b"[spore] integration regression: PASS",
        "guest did not report a passing integration run",
    ),
    (
        b"[spore] stdin demo: blocking read resume: PASS",
        "guest did not resume its blocked stdin reader",
    ),
];

pub fn run(argv: &[OsString], mode: &str, timings: bool) -> io::Result<i32> {
    let mut first = String::new();
    let status = run_inner(argv, mode, timings, &mut first)?;
    if mode != "rng" || status != 0 {
        return Ok(status);
    }
    let mut second = String::new();
    let status = run_inner(argv, mode, timings, &mut second)?;
    if status == 0 {
        if first == second {
            return Err(io::Error::other(
                "repeated /dev/urandom sample across boots",
            ));
        }
        println!("rng: PASS samples differ across boots");
    }
    Ok(status)
}

fn run_inner(
    argv: &[OsString],
    mode: &str,
    mut timings: bool,
    sample: &mut String,
) -> io::Result<i32> {
    let (mut serial, writer) = io::pipe()?;
    let child = Command::new(&argv[0])
        .args(&argv[1..])
        .stdin(Stdio::piped())
        .stdout(writer.try_clone()?)
        .stderr(writer)
        .spawn();
    let mut vm = match child {
        Ok(child) => Vm(child),
        Err(e) => {
            eprintln!("{}: {e}", argv[0].to_string_lossy());
            return Ok(127);
        }
    };
    let mut input = SerialInput(vm.0.stdin.take().unwrap());
    let plain = mode == "plain";
    let shell = mode == "shell";
    let filtered = matches!(mode, "filter" | "integration" | "stdin");
    let terminal = Terminal::open(plain);
    let mut keyboard = if plain {
        Some(File::from(io::stdin().as_fd().try_clone_to_owned()?))
    } else {
        None
    };
    let mut check = modes::ShellCheck::new();
    let mut bench = modes::Bench::default();
    let mut rng = modes::Rng::default();
    let mut buf = SerialWindow::new(262143);
    let mut startup = SerialWindow::new(32768);
    let mut passed = [false; 2];
    let mut screen = (plain && io::stdout().is_terminal()).then(TerminalOutput::default);
    let mut output = File::from(io::stdout().as_fd().try_clone_to_owned()?);
    let mut handed_off = false;
    let mut sized = false;
    let mut answered = false;
    let start = Instant::now();
    let timeout = Duration::from_secs(if shell { 300 } else { 120 });
    let mut milestones = [None; 6];
    let mut first = None;
    let mut serial_open = true;
    if timings {
        eprintln!("[host] qemu launched; timing summary prints at shell prompt\r");
    }
    let mut chunk = [0; 4096];
    let code = loop {
        let sig = unsafe { runner_interrupted() };
        if sig != 0 {
            break 128 + sig;
        }
        if !plain && start.elapsed() > timeout {
            break 124;
        }
        if sized && unsafe { runner_resized() } {
            terminal.send_size(&mut input)?;
        }
        let events = unsafe {
            runner_poll(
                if serial_open { serial.as_raw_fd() } else { -1 },
                keyboard.as_ref().map_or(-1, AsRawFd::as_raw_fd),
                50,
            )
        };
        if events < 0 {
            return Err(io::Error::last_os_error());
        }
        if events & 2 != 0
            && let Some(k) = keyboard.as_mut()
        {
            let n = k.read(&mut chunk)?;
            if n == 0 {
                keyboard = None;
            } else {
                input.write_all(&chunk[..n])?;
            }
        }
        if events & 1 != 0 {
            let mut n = serial.read(&mut chunk)?;
            if screen.is_some() && n != 0 {
                let deadline = Instant::now() + Duration::from_millis(2);
                while n < chunk.len() && Instant::now() < deadline {
                    if unsafe { runner_poll(serial.as_raw_fd(), -1, 1) } <= 0 {
                        break;
                    }
                    let got = serial.read(&mut chunk[n..])?;
                    if got == 0 {
                        break;
                    }
                    n += got;
                }
            }
            if n == 0 {
                serial_open = false;
            } else {
                let data = &chunk[..n];
                let now = Instant::now();
                check.last_output = now;
                if !plain || !sized || timings {
                    buf.push(data);
                }
                if mode == "integration" {
                    for (seen, (marker, _)) in passed.iter_mut().zip(INTEGRATION_CHECKS) {
                        if !*seen {
                            *seen = buf.find_new(marker).is_some();
                        }
                    }
                }
                if timings {
                    let t = start.elapsed().as_secs_f64();
                    first.get_or_insert(t);
                    for (i, (needle, _)) in MARKERS.iter().enumerate() {
                        if milestones[i].is_none() && buf.find_new(needle).is_some() {
                            milestones[i] = Some(t);
                        }
                    }
                    if milestones[5].is_some() {
                        eprintln!(
                            "\r\n[host] boot timings:\r\n[host]   +{:.3}s first qemu output\r",
                            first.unwrap()
                        );
                        for (i, (_, label)) in MARKERS.iter().enumerate() {
                            if let Some(t) = milestones[i] {
                                eprintln!("[host]   +{t:.3}s {label}\r");
                            }
                        }
                        timings = false;
                    }
                }
                if plain || shell {
                    let visible = if handed_off {
                        data
                    } else {
                        startup.push(data);
                        if let Some(i) = startup
                            .find_new(b"[kernel] Limine handoff")
                            .or_else(|| startup.prompt())
                        {
                            handed_off = true;
                            &startup.bytes()[i..]
                        } else {
                            &[]
                        }
                    };
                    if let Some(screen) = &mut screen {
                        screen.relay(&mut output, visible)?;
                    } else {
                        output.write_all(visible)?;
                    }
                }
                if !sized && buf.prompt().is_some() {
                    terminal.send_size(&mut input)?;
                    sized = true;
                }
                if !answered && matches!(mode, "integration" | "stdin") {
                    answered = modes::answer_guest_stdin(&mut input, &buf)?;
                }
                if shell && !check.output(&mut input, &mut buf)? {
                    break 1;
                }
                if matches!(mode, "bench" | "rng") && buf.prompt().is_some() {
                    if mode == "bench" {
                        bench.prompt(&mut input)?;
                    } else {
                        rng.prompt(&mut input, buf.bytes())?;
                    }
                    buf.clear();
                }
                continue;
            }
        } else if shell {
            check.idle(&mut input, &mut buf)?;
        }
        if let Some(status) = vm.0.try_wait()? {
            break status.code().unwrap_or(128);
        }
    };
    if !handed_off && (plain || shell) {
        io::stdout().write_all(startup.bytes())?;
    }
    if let Some(screen) = &mut screen {
        screen.finish(&mut output)?;
    }
    if filtered {
        modes::print_guest_checks(buf.bytes());
    }
    if mode == "rng" && code == 0 {
        if rng.sample.is_empty() {
            return Err(io::Error::other("missing /dev/urandom sample"));
        }
        *sample = rng.sample.clone();
        println!("[rng] {}", rng.sample);
    }
    if code == 0 && mode == "integration" {
        for (seen, (_, message)) in passed.iter().zip(INTEGRATION_CHECKS) {
            if !seen {
                eprintln!("spore: {message}");
                return Ok(1);
            }
        }
    }
    if code == 0 && shell && !check.completed() {
        eprintln!("spore: guest exited before the shell check reached shutdown");
        return Ok(1);
    }
    Ok(code)
}
