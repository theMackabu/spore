#![no_std]

#[path = "../../lib/spore/panic.rs"]
mod panic;

#[repr(C)]
pub enum UnitType {
    Service,
    Target,
    Timer,
    Path,
    Socket,
    Mount,
}
#[repr(C)]
pub enum ServiceType {
    Simple,
    Forking,
    Oneshot,
    Notify,
    Idle,
}
#[repr(C)]
pub enum Restart {
    No,
    OnSuccess,
    OnFailure,
    OnAbnormal,
    OnWatchdog,
    Always,
}
#[repr(C)]
struct Words {
    items: [[u8; 64]; 12],
    count: i32,
}

#[repr(C)]
pub struct UnitConfig {
    kind: UnitType,
    service_type: ServiceType,
    restart: Restart,
    description: [u8; 96],
    exec_start: [u8; 192],
    exec_start_pre: [u8; 192],
    exec_start_post: [u8; 192],
    exec_stop: [u8; 192],
    exec_reload: [u8; 192],
    capability: [u8; 64],
    user: [u8; 32],
    standard_output: [u8; 96],
    standard_error: [u8; 96],
    wanted_by: [u8; 64],
    unit_to_activate: [u8; 64],
    requires_units: Words,
    wants: Words,
    requisite: Words,
    binds_to: Words,
    part_of: Words,
    conflicts: Words,
    after: Words,
    before: Words,
    restart_sec: i32,
    restart_immediately: bool,
    watchdog_sec: i32,
    timeout_stop_sec: i32,
    start_limit_interval: i32,
    start_limit_burst: i32,
    on_boot_sec: i32,
    on_unit_active_sec: i32,
    on_unit_inactive_sec: i32,
    on_calendar_minute: i32,
    cpu_budget_ticks: u64,
    memory_pages: u64,
    persistent: bool,
}
fn store(dst: &mut [u8], src: &[u8]) {
    let n = src.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&src[..n]);
    dst[n] = 0;
}
impl Words {
    fn append(&mut self, value: &[u8]) {
        for word in value[..value.len().min(255)]
            .split(|&c| c == b' ' || c == b'\t')
            .filter(|w| !w.is_empty())
        {
            let Some(slot) = self.items.get_mut(self.count as usize) else {
                break;
            };
            store(slot, word);
            self.count += 1;
        }
    }
}

fn trim(s: &[u8]) -> &[u8] {
    let space = |c: &u8| matches!(c, b' ' | b'\r' | b'\t' | b'\n');
    let start = s.iter().position(|c| !space(c)).unwrap_or(s.len());
    let end = s.iter().rposition(|c| !space(c)).map_or(start, |i| i + 1);
    &s[start..end]
}

fn seconds(v: &[u8]) -> i32 {
    let v = v.strip_suffix(b"s").unwrap_or(v);
    core::str::from_utf8(v)
        .ok()
        .and_then(|s| s.parse::<i32>().ok())
        .filter(|n| (0..=86400).contains(n))
        .unwrap_or(0)
}

fn decimal(v: &[u8]) -> (bool, u64, bool) {
    let v = trim(v);
    let neg = v.first() == Some(&b'-');
    let v = if matches!(v.first(), Some(b'-' | b'+')) {
        &v[1..]
    } else {
        v
    };
    let mut n = 0u64;
    for &b in v.iter().take_while(|b| b.is_ascii_digit()) {
        let Some(next) = n
            .checked_mul(10)
            .and_then(|k| k.checked_add((b - b'0') as u64))
        else {
            return (neg, u64::MAX, true);
        };
        n = next;
    }
    (neg, n, false)
}
fn integer(v: &[u8]) -> i32 {
    let (neg, n, _) = decimal(v);
    if neg {
        (n as i32).wrapping_neg()
    } else {
        n as i32
    }
}

#[repr(u8)]
#[derive(Clone, Copy)]
pub enum Section {
    Unit,
    Service,
    Timer,
    Install,
    Unknown,
}

fn apply(c: &mut UnitConfig, section: Section, key: &[u8], v: &[u8]) {
    match section {
        Section::Unit => match key {
            b"Description" => store(&mut c.description, v),
            b"Requires" => c.requires_units.append(v),
            b"Wants" => c.wants.append(v),
            b"Requisite" => c.requisite.append(v),
            b"BindsTo" => c.binds_to.append(v),
            b"PartOf" => c.part_of.append(v),
            b"Conflicts" => c.conflicts.append(v),
            b"After" => c.after.append(v),
            b"Before" => c.before.append(v),
            _ => (),
        },
        Section::Service => match key {
            b"Type" => {
                c.service_type = match v {
                    b"oneshot" => ServiceType::Oneshot,
                    b"forking" => ServiceType::Forking,
                    b"notify" => ServiceType::Notify,
                    b"idle" => ServiceType::Idle,
                    _ => ServiceType::Simple,
                }
            }
            b"Restart" => {
                c.restart = match v {
                    b"on-success" => Restart::OnSuccess,
                    b"on-failure" => Restart::OnFailure,
                    b"on-abnormal" => Restart::OnAbnormal,
                    b"on-watchdog" => Restart::OnWatchdog,
                    b"always" => Restart::Always,
                    _ => Restart::No,
                }
            }
            b"ExecStart" => store(&mut c.exec_start, v),
            b"ExecStartPre" => store(&mut c.exec_start_pre, v),
            b"ExecStartPost" => store(&mut c.exec_start_post, v),
            b"ExecStop" => store(&mut c.exec_stop, v),
            b"ExecReload" => store(&mut c.exec_reload, v),
            b"RestartSec" => c.restart_sec = seconds(v),
            b"RestartImmediately" => c.restart_immediately = matches!(v, b"yes" | b"true" | b"1"),
            b"StartLimitIntervalSec" => c.start_limit_interval = seconds(v),
            b"StartLimitBurst" => c.start_limit_burst = integer(v),
            b"WatchdogSec" => c.watchdog_sec = seconds(v),
            b"TimeoutStopSec" => c.timeout_stop_sec = seconds(v),
            b"Capability" => store(&mut c.capability, v),
            b"CPUBudget" => c.cpu_budget_ticks = seconds(v) as u64,
            b"MemoryPages" => {
                let (neg, n, overflow) = decimal(v);
                c.memory_pages = if neg && !overflow {
                    n.wrapping_neg()
                } else {
                    n
                };
            }
            b"User" => store(&mut c.user, v),
            b"StandardOutput" => store(&mut c.standard_output, v),
            b"StandardError" => store(&mut c.standard_error, v),
            _ => (),
        },
        Section::Timer => {
            c.kind = UnitType::Timer;
            match key {
                b"OnBootSec" => c.on_boot_sec = seconds(v),
                b"OnUnitActiveSec" => c.on_unit_active_sec = seconds(v),
                b"OnUnitInactiveSec" => c.on_unit_inactive_sec = seconds(v),
                b"OnCalendar" => {
                    c.on_calendar_minute =
                        integer(v.iter().position(|&b| b == b':').map_or(v, |i| &v[i + 1..]))
                }
                b"Persistent" => c.persistent = matches!(v, b"yes" | b"true"),
                b"Unit" => store(&mut c.unit_to_activate, v),
                _ => (),
            }
        }
        Section::Install if matches!(key, b"WantedBy" | b"RequiredBy") => {
            store(&mut c.wanted_by, v)
        }
        _ => (),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mycelium_parse_line(
    config: *mut UnitConfig,
    section: *mut Section,
    line: *const u8,
    len: usize,
) {
    let line = trim(unsafe { core::slice::from_raw_parts(line, len) });
    if line.is_empty() || line[0] == b'#' {
        return;
    }
    if line[0] == b'[' {
        if let Some(end) = line.iter().position(|&b| b == b']') {
            unsafe {
                *section = match &line[1..end] {
                    b"Unit" => Section::Unit,
                    b"Service" => Section::Service,
                    b"Timer" => Section::Timer,
                    b"Install" => Section::Install,
                    _ => Section::Unknown,
                }
            };
        }
    } else if let Some(eq) = line.iter().position(|&b| b == b'=') {
        apply(
            unsafe { &mut *config },
            unsafe { *section },
            trim(&line[..eq]),
            trim(&line[eq + 1..]),
        );
    }
}
