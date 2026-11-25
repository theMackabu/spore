use super::cpu::{Cpu, Frame};
use std::alloc::{Layout, alloc_zeroed};
use std::cell::RefCell;

unsafe extern "C" {
    fn wasm_boot(
        image: *const u8,
        n: usize,
        args: *const u8,
        len: usize,
        devices: u32,
        epoch: u64,
        ram: *mut u8,
        ram_bytes: usize,
    ) -> *const Frame;
}
thread_local! {
    static INPUT:RefCell<Vec<u8>>=const{RefCell::new(Vec::new())};
    static CPU:RefCell<Option<Cpu>>=const{RefCell::new(None)};
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
pub extern "C" fn start(size: usize, devices: u32, epoch: u64, ram_mib: u32) -> bool {
    CPU.with_borrow_mut(|cpu| {
        INPUT.with_borrow(|image| {
            if (size == 0 && (devices & 1 == 0 || !image.is_empty()))
                || (size != 0 && size < 64)
                || size > image.len()
                || !(16..2048).contains(&ram_mib)
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
                )
            };
            *cpu = Some(Cpu::new(unsafe { *first }));
            true
        })
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn run_slice(budget: u32, ticks: u32) -> i32 {
    CPU.with_borrow_mut(|state| {
        let Some(cpu) = state else {
            return -1;
        };
        if ticks != 0 {
            cpu.tick(ticks);
        }
        for retired in 0..budget {
            if cpu.idle {
                return retired as i32;
            }
            if let Err(error) = cpu.step() {
                cpu.exception(error);
            }
        }
        budget as i32
    })
}
