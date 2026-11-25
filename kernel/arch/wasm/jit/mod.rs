mod compiler;

use super::cpu::{Cpu, Frame};

#[link(wasm_import_module = "spore")]
unsafe extern "C" {
    fn jit_compile(slot: usize, slots: usize, module: *const u8, size: usize) -> usize;
}

type Compiled =
    unsafe extern "C" fn(&mut Frame, u32, *const super::memory::Translation, u32) -> u32;

thread_local! {
    static INSTRUCTIONS:std::cell::Cell<u64> = const{std::cell::Cell::new(0)};
}

#[unsafe(no_mangle)]
pub extern "C" fn compiled_instructions() -> u64 {
    INSTRUCTIONS.get()
}

#[derive(Default)]
struct Entry {
    pc: u64,
    hits: u16,
    compiled: Option<Compiled>,
    syscall_return: Option<u64>,
    code: Vec<u32>,
}

pub struct Cache {
    entries: Vec<Entry>,
    pub threshold: u16,
}

impl Cache {
    pub fn new() -> Self {
        Self {
            threshold: 512,
            entries: (0..1024).map(|_| Entry::default()).collect(),
        }
    }

    pub fn run(&mut self, cpu: &mut Cpu, budget: u32) -> u32 {
        let pc = cpu.f.pc;
        if pc & 3 != 0 || cpu.reservation.is_some() {
            return 0;
        }
        let index = ((pc >> 2) ^ (pc >> 11)) as usize % self.entries.len();
        let slots = self.entries.len();
        let entry = &mut self.entries[index];
        if entry.pc != pc {
            entry.pc = pc;
            entry.code.clear();
            entry.hits = 0;
        }
        if entry.hits < self.threshold {
            entry.hits += 1;
            return 0;
        }
        if entry.hits == u16::MAX {
            return 0;
        }
        let Some(page) = cpu.memory.code(pc) else {
            return 0;
        };
        if !entry.code.is_empty() && !page.starts_with(&entry.code) {
            entry.code.clear();
            entry.hits = 0;
            return 0;
        }
        if entry.code.is_empty() {
            let Some((module, count)) = compiler::compile(pc, &page[..page.len().min(48)]) else {
                entry.hits = u16::MAX;
                return 0;
            };
            let address = unsafe { jit_compile(index, slots, module.as_ptr(), module.len()) };
            if address == 0 {
                entry.hits = u16::MAX;
                return 0;
            }
            entry.compiled = Some(unsafe { std::mem::transmute::<usize, Compiled>(address) });
            entry.code.extend_from_slice(&page[..count]);
            entry.syscall_return = matches!(
                super::decode::classify(page[count - 1]),
                super::decode::Op::Syscall
            )
            .then_some(pc + 4 * count as u64);
        }
        if budget < entry.code.len() as u32 {
            return 0;
        }
        let code_page = page.as_ptr() as u32 & !4095;
        let retired =
            unsafe { (entry.compiled.unwrap())(&mut cpu.f, budget, cpu.memory.table(), code_page) };
        if retired != 0 && entry.syscall_return == Some(cpu.f.pc) {
            cpu.syscall();
        }
        INSTRUCTIONS.set(INSTRUCTIONS.get() + u64::from(retired));
        retired
    }
}
