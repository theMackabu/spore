mod compiler;

use super::cpu::{Cpu, Frame};

#[link(wasm_import_module = "spore")]
unsafe extern "C" {
    fn jit_compile(slot: usize, module: *const u8, size: usize) -> bool;
    fn jit_run(slot: usize, frame: &mut Frame, budget: u32) -> u32;
}

#[derive(Default)]
struct Entry {
    pc: u64,
    hits: u16,
    code: Vec<u32>,
}

pub struct Cache {
    entries: Vec<Entry>,
}

impl Cache {
    pub fn new() -> Self {
        Self {
            entries: (0..256).map(|_| Entry::default()).collect(),
        }
    }

    pub fn run(&mut self, cpu: &mut Cpu, budget: u32) -> u32 {
        let pc = cpu.f.pc;
        if pc & 3 != 0 {
            return 0;
        }
        let index = ((pc >> 2) ^ (pc >> 11)) as usize % self.entries.len();
        let entry = &mut self.entries[index];
        if entry.pc != pc {
            entry.pc = pc;
            entry.code.clear();
            entry.hits = 0;
        }
        if entry.hits < 512 {
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
            if !unsafe { jit_compile(index, module.as_ptr(), module.len()) } {
                entry.hits = u16::MAX;
                return 0;
            }
            entry.code.extend_from_slice(&page[..count]);
        }
        if budget < entry.code.len() as u32 {
            return 0;
        }
        unsafe { jit_run(index, &mut cpu.f, budget) }
    }
}
