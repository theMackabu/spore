mod compiler;

use super::cpu::Cpu;

#[link(wasm_import_module = "spore")]
unsafe extern "C" {
    fn jit_compile(slot: usize, slots: usize, module: *const u8, size: usize) -> usize;
}

type Compiled = unsafe extern "C" fn(*mut Cpu, u32, *const super::memory::Translation, u32) -> u32;

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
    epoch: u64,
    code: Vec<(u64, Vec<u32>)>,
    count: u32,
}

const SETS: usize = 1024;
const WAYS: usize = 4;

#[derive(Clone, Copy, Default)]
struct Dispatch {
    pc: u64,
    epoch: u64,
    count: u32,
    function: Option<Compiled>,
}

pub struct Cache {
    entries: Vec<Entry>,
    victims: Vec<u8>,
    dispatch: Vec<Dispatch>,
}

impl Cache {
    pub fn new() -> Self {
        Self {
            entries: (0..SETS * WAYS).map(|_| Entry::default()).collect(),
            victims: vec![0; SETS],
            dispatch: vec![Dispatch::default(); SETS],
        }
    }

    #[inline(always)]
    pub fn run(&mut self, cpu: &mut Cpu, budget: u32) -> u32 {
        let pc = cpu.f.pc;
        if pc & 3 != 0 || cpu.reservation.is_some() {
            return 0;
        }
        let set = ((pc >> 2) ^ (pc >> 12) ^ (pc >> 22)) as usize & (SETS - 1);
        let slot = self.dispatch[set];
        let epoch = super::memory::code_epoch();
        let function = if slot.pc == pc && slot.epoch == epoch && budget >= slot.count {
            slot.function
        } else {
            self.lookup(cpu, budget, pc, set, epoch)
        };
        let Some(function) = function else {
            return 0;
        };
        let result = unsafe { function(cpu, budget, cpu.memory.table(), 0) };
        let retired = result & i32::MAX as u32;
        if result != retired {
            cpu.syscall();
        }
        INSTRUCTIONS.set(INSTRUCTIONS.get() + u64::from(retired));
        retired
    }

    #[cold]
    fn lookup(
        &mut self,
        cpu: &mut Cpu,
        budget: u32,
        pc: u64,
        set: usize,
        epoch: u64,
    ) -> Option<Compiled> {
        self.dispatch[set].function = None;
        self.dispatch[set].epoch = 0;
        let base = set * WAYS;
        let way = self.entries[base..base + WAYS]
            .iter()
            .position(|e| e.pc == pc)
            .unwrap_or_else(|| {
                let victim = self.victims[set];
                self.victims[set] = (victim + 1) % WAYS as u8;
                usize::from(victim)
            });
        let index = base + way;
        let slots = self.entries.len();
        let entry = &mut self.entries[index];
        if entry.pc != pc {
            entry.pc = pc;
            entry.code.clear();
            entry.hits = 0;
        }
        if entry.hits < 256 {
            entry.hits += 1;
            return None;
        }
        if entry.hits == u16::MAX {
            return None;
        }
        if entry.epoch != epoch {
            if entry.code.iter().any(|(pc, code)| {
                !cpu.memory
                    .trace_words(*pc)
                    .is_some_and(|page| page.starts_with(code))
            }) {
                entry.code.clear();
                entry.hits = 0;
                return None;
            }
            entry.epoch = epoch;
        }
        if entry.code.is_empty() {
            let Some(trace) = compiler::compile(pc, &mut cpu.memory, &cpu.branches) else {
                entry.hits = u16::MAX;
                return None;
            };
            let address =
                unsafe { jit_compile(index, slots, trace.module.as_ptr(), trace.module.len()) };
            if address == 0 {
                entry.hits = u16::MAX;
                return None;
            }
            entry.compiled = Some(unsafe { std::mem::transmute::<usize, Compiled>(address) });
            entry.code = trace.spans;
            entry.count = trace.count;
        }
        if budget < entry.count {
            return None;
        }
        self.dispatch[set] = Dispatch {
            pc,
            epoch,
            count: entry.count,
            function: entry.compiled,
        };
        entry.compiled
    }
}
