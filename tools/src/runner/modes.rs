use super::serial::SerialWindow;
use std::fmt::Write as _;
use std::io::{self, Write};
use std::time::{Duration, Instant};

#[path = "commands.rs"]
mod commands;
use commands::COMMANDS;

fn contains(buf: &[u8], needle: &[u8]) -> bool {
    buf.windows(needle.len()).any(|w| w == needle)
}

pub fn print_guest_checks(buf: &[u8]) {
    let mut out = io::stdout().lock();
    let mut matched = false;
    for line in buf.split(|&c| c == b'\n') {
        if [b"[spore]" as &[u8], b"[cell ", b"[kernel] lower sync fault"]
            .iter()
            .any(|n| contains(line, n))
        {
            let _ = out.write_all(line);
            let _ = out.write_all(b"\n");
            matched = true;
        }
    }
    if !matched {
        let _ = out.write_all(buf);
    }
}

pub fn answer_guest_stdin(input: &mut impl Write, buf: &SerialWindow) -> io::Result<bool> {
    let ready = buf
        .find_new(b"[spore] stdin demo: child blocking on read(0)")
        .is_some();
    if ready {
        input.write_all(b"z\n")?;
    }
    Ok(ready)
}

#[derive(Default)]
pub struct Bench {
    index: usize,
    started: Option<Instant>,
    shutdown: bool,
}

impl Bench {
    pub fn prompt(&mut self, input: &mut impl Write) -> io::Result<()> {
        const TESTS: &[(&str, &str)] = &[
            ("ant startup", "time ant -e 'console.log(1)' > /dev/null"),
            ("ls /bin", "time ls /bin > /dev/null"),
            ("du -s /", "time du -s / > /dev/null"),
            ("find /etc", "time find /etc > /dev/null"),
            ("ls -l usr/local", "time ls -l /usr/local/bin > /dev/null"),
            ("dynamic opens", "time curl -V > /dev/null"),
            (
                "curl http",
                "time curl -s http://10.0.2.2:8080/ > /dev/null",
            ),
            (
                "curl https",
                "time curl -sS https://example.com/ > /dev/null",
            ),
            ("nano --version", "time nano --version > /dev/null"),
            ("fs stats", "cat /proc/fsstats"),
        ];
        if let Some(start) = self.started.take() {
            println!(
                "[bench] {:<18} {:.3}s",
                TESTS[self.index].0,
                start.elapsed().as_secs_f64()
            );
            self.index += 1;
        }
        if let Some((label, cmd)) = TESTS.get(self.index) {
            println!("[bench] running {label:<18}");
            io::stdout().flush()?;
            self.started = Some(Instant::now());
            writeln!(input, "{cmd}")?;
        } else if !self.shutdown {
            input.write_all(b"sudo shutdown\n")?;
            self.shutdown = true;
        }
        Ok(())
    }
}

#[derive(Default)]
pub struct Rng {
    stage: u8,
    pub sample: String,
}

impl Rng {
    pub fn prompt(&mut self, input: &mut impl Write, buf: &[u8]) -> io::Result<()> {
        match self.stage {
            0 => input.write_all(b"xxd -l 32 /dev/urandom\n")?,
            1 => {
                if let Some(i) = buf.windows(9).position(|v| v == b"00000000:") {
                    let line = buf[i..].split(|&c| c == b'\n').next().unwrap_or_default();
                    self.sample =
                        String::from_utf8_lossy(&line[..line.len().min(255)]).into_owned();
                }
                input.write_all(b"sudo shutdown\n")?;
            }
            _ => return Ok(()),
        }
        self.stage += 1;
        Ok(())
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Script {
    None,
    EditMotd,
    EditPaste,
    Pico,
    PicoPaste,
    NanoPaste,
    AntFirst,
    AntSecond,
    AntExit,
}

pub struct ShellCheck {
    sent: usize,
    script: Script,
    nudge: bool,
    pub last_output: Instant,
}

fn numbered(prefix: &str, tag: &str, count: usize, sep: &str, suffix: &str) -> String {
    let mut s = String::from(prefix);
    for i in 1..=count {
        write!(s, "{tag}-{i:03}{sep}").unwrap();
    }
    s.push_str(suffix);
    s
}

impl ShellCheck {
    pub fn new() -> Self {
        Self {
            sent: 0,
            script: Script::None,
            nudge: false,
            last_output: Instant::now(),
        }
    }
    pub fn output(&mut self, input: &mut impl Write, buf: &mut SerialWindow) -> io::Result<bool> {
        if buf.find_new(b"__SPORE_SHELL_CHECK_FAIL_").is_some() {
            return Ok(false);
        }
        let response = match self.script {
            Script::Pico if buf.find_new(b"GNU nano").is_some() => {
                Some("from pico\x0f\r\x18".to_owned())
            }
            Script::PicoPaste if buf.find_new(b"GNU nano").is_some() => {
                Some(numbered("", "pico-paste", 32, "\r", "\x0f\r\x18"))
            }
            Script::NanoPaste if buf.find_new(b"GNU nano").is_some() => {
                Some(numbered("", "nano-paste", 32, "\r", "\x0f\r\x18"))
            }
            Script::EditMotd if buf.find_new(b"ed-ready>").is_some() => {
                Some("1d\na\nshell-check motd\n.\nw\nq\n".to_owned())
            }
            Script::EditPaste if buf.find_new(b"ed-ready>").is_some() => {
                Some(numbered("a\n", "edit-paste", 64, "\n", ".\nw\nq\n"))
            }
            _ => None,
        };
        if let Some(keys) = response {
            input.write_all(keys.as_bytes())?;
            self.nudge = matches!(
                self.script,
                Script::Pico | Script::PicoPaste | Script::NanoPaste
            );
            self.script = Script::None;
            buf.clear();
        }
        if let Some(cmd) = COMMANDS.get(self.sent).filter(|_| buf.prompt().is_some()) {
            if matches!(self.script, Script::AntFirst | Script::AntSecond) {
                return Ok(false);
            }
            self.nudge = false;
            self.script = match *cmd {
                "sudo edit -p 'ed''-ready>' /etc/motd\n" => Script::EditMotd,
                "edit -p 'ed''-ready>' /tmp/edit-paste\n" => Script::EditPaste,
                "pico /tmp/pico-test\n" => Script::Pico,
                "pico /tmp/pico-paste\n" => Script::PicoPaste,
                "nano /tmp/nano-paste\n" => Script::NanoPaste,
                "ant\n" => Script::AntFirst,
                _ => Script::None,
            };
            if *cmd == "__spore_harness_shell_paste__\n" {
                input.write_all(
                    numbered("printf '", "shell-paste", 64, "\\n", "' > /tmp/shell-paste\n").as_bytes(),
                )?;
            } else {
                input.write_all(cmd.as_bytes())?;
            }
            self.sent += 1;
            buf.clear();
        }
        Ok(true)
    }
    pub fn completed(&self) -> bool {
        self.sent == COMMANDS.len()
    }

    pub fn idle(&mut self, input: &mut impl Write, buf: &mut SerialWindow) -> io::Result<()> {
        if matches!(self.script, Script::AntFirst | Script::AntSecond)
            && contains(buf.bytes(), "❯".as_bytes())
            && self.last_output.elapsed() > Duration::from_millis(300)
            && (self.script == Script::AntFirst || contains(buf.bytes(), b"press Ctrl+C again"))
        {
            input.write_all(b"\x03")?;
            self.script = if self.script == Script::AntFirst {
                Script::AntSecond
            } else {
                Script::AntExit
            };
            buf.clear();
        } else if self.nudge && self.last_output.elapsed() > Duration::from_secs(1) {
            input.write_all(b"\n")?;
            self.nudge = false;
        }
        Ok(())
    }
}
