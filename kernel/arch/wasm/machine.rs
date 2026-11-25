use super::cpu::{Cpu, Frame};
use std::alloc::{Layout, alloc_zeroed};
use std::cell::RefCell;

unsafe extern "C" {
    fn wasm_shutdown_storage() -> bool;
    fn wasm_select_cpu(cpu: u32);
    fn wasm_boot(
        image: *const u8,
        n: usize,
        args: *const u8,
        len: usize,
        devices: u32,
        epoch: u64,
        ram: *mut u8,
        ram_bytes: usize,
        count: u32,
    ) -> *const Frame;
}
struct Machine {
    jit: Option<super::jit::Cache>,
    cpus: Vec<Cpu>,
    next: usize,
}

thread_local! {
    static INPUT:RefCell<Vec<u8>>=const{RefCell::new(Vec::new())};
    static CPU:RefCell<Option<Machine>>=const{RefCell::new(None)};
}
#[unsafe(no_mangle)]
pub extern "C" fn stage(size: usize) -> *mut u8 {
    INPUT.with_borrow_mut(|image| {
        if size == 0 || !image.is_empty() || image.try_reserve_exact(size).is_err() {
            return std::ptr::null_mut();
        }
        image.resize(size, 0);
        image.as_mut_ptr()
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn start(size: usize, devices: u32, epoch: u64, ram_mib: u32, count: u32) -> bool {
    CPU.with_borrow_mut(|cpu| {
        INPUT.with_borrow(|image| {
            if (size == 0 && (devices & 1 == 0 || !image.is_empty()))
                || (size != 0 && size < 64)
                || size > image.len()
                || !(16..2048).contains(&ram_mib)
                || !(1..=64).contains(&count)
                || cpu.is_some()
            {
                return false;
            }
            let ram_bytes = (ram_mib as usize) << 20;
            let Ok(layout) = Layout::from_size_align(ram_bytes, 4096) else {
                return false;
            };
            let ram = unsafe { alloc_zeroed(layout) };
            if ram.is_null() {
                return false;
            }
            let first = unsafe {
                wasm_boot(
                    image.as_ptr(),
                    size,
                    image[size..].as_ptr(),
                    image.len() - size,
                    devices,
                    epoch,
                    ram,
                    ram_bytes,
                    count,
                )
            };
            let frame = unsafe { *first };
            let cpus = (0..count)
                .map(|id| {
                    let mut cpu = Cpu::new(frame);
                    cpu.idle = id != 0;
                    cpu
                })
                .collect();
            *cpu = Some(Machine {
                cpus,
                next: 0,
                jit: (devices & 16 != 0).then(super::jit::Cache::new),
            });
            true
        })
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn stop() -> bool {
    CPU.with_borrow(|state| state.is_some() && unsafe { wasm_shutdown_storage() })
}

#[unsafe(no_mangle)]
pub extern "C" fn run_slice(budget: u32, ticks: u32) -> i32 {
    CPU.with_borrow_mut(|state| {
        let Some(machine) = state else {
            return -1;
        };
        let count = machine.cpus.len();
        if ticks != 0 {
            for (id, cpu) in machine.cpus.iter_mut().enumerate() {
                unsafe {
                    wasm_select_cpu(id as u32);
                }
                cpu.tick(ticks);
            }
        }
        let mut retired = 0;
        let mut inactive = 0;
        while retired < budget && inactive < count {
            let id = machine.next;
            let cpu = &mut machine.cpus[id];
            if count > 1 {
                unsafe {
                    wasm_select_cpu(id as u32);
                }
                cpu.tick(0);
                machine.next = (id + 1) % count;
            }
            if cpu.idle {
                inactive += 1;
                continue;
            }
            inactive = 0;
            let end = if count == 1 {
                budget
            } else {
                budget.min(retired + 4096)
            };
            let mut entry = true;
            while retired < end && !cpu.idle {
                if entry && let Some(jit) = machine.jit.as_mut() {
                    let compiled = jit.run(cpu, end - retired);
                    if compiled != 0 {
                        retired += compiled;
                        continue;
                    }
                }
                let before = cpu.f.pc;
                if let Err(error) = cpu.step() {
                    cpu.exception(error);
                }
                entry = before.wrapping_sub(cpu.f.pc) < 48 * 4;
                retired += 1;
            }
        }
        retired as i32
    })
}
