use super::cpu::{Cpu, Frame};
use std::cell::RefCell;

unsafe extern "C" {
    fn wasm_boot(
        image: *const u8,
        n: usize,
        args: *const u8,
        len: usize,
        disk: bool,
        epoch: u64,
    ) -> *const Frame;
}
#[link(wasm_import_module = "spore")]
unsafe extern "C" {
    fn fault(pc: u64, msg: *const u8, len: usize);
}
thread_local! {
    static INPUT:RefCell<Vec<u8>>=const{RefCell::new(Vec::new())};
    static CPU:RefCell<Option<Cpu>>=const{RefCell::new(None)};
}
#[unsafe(no_mangle)]
pub extern "C" fn stage(size: usize) -> *mut u8 {
    INPUT.with_borrow_mut(|image| {
        if size == 0 || size > 128 << 20 || !image.is_empty() {
            return std::ptr::null_mut();
        }
        image.resize(size, 0);
        image.as_mut_ptr()
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn start(size: usize, disk: bool, epoch: u64) -> bool {
    CPU.with_borrow_mut(|cpu| {
        INPUT.with_borrow(|image| {
            if size < 64 || size > image.len() || cpu.is_some() {
                return false;
            }
            let first = unsafe {
                wasm_boot(
                    image.as_ptr(),
                    size,
                    image[size..].as_ptr(),
                    image.len() - size,
                    disk,
                    epoch,
                )
            };
            *cpu = Some(Cpu::new(unsafe { *first }));
            true
        })
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn run_slice(budget: u32) -> bool {
    CPU.with_borrow_mut(|state| {
        let Some(cpu) = state else {
            return false;
        };
        for _ in 0..budget {
            if let Err(error) = cpu.step() {
                let message = error.to_string();
                unsafe { fault(cpu.f.pc, message.as_ptr(), message.len()) };
                return false;
            }
        }
        true
    })
}
