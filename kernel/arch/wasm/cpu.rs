use super::memory::{Access, Memory};

#[derive(Debug)]
pub enum Fault {
    Instruction(u32),
    Breakpoint,
    Alignment(u64),
    Memory(Access, u64),
}
impl std::fmt::Display for Fault {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Breakpoint => write!(f, "ARM64 breakpoint"),
            Self::Instruction(word) => write!(f, "unsupported A64 instruction {word:#010x}"),
            Self::Alignment(addr) => write!(f, "unaligned address {addr:#x}"),
            Self::Memory(access, addr) => write!(f, "{access:?} fault at {addr:#x}"),
        }
    }
}

#[repr(C, align(16))]
#[derive(Clone, Copy)]
pub struct Frame {
    pub x: [u64; 31],
    pub sp: u64,
    pub pc: u64,
    pub pstate: u64,
    esr: u64,
    pad: u64,
}
const _: () = assert!(std::mem::size_of::<Frame>() == 288);

#[repr(C, align(16))]
#[derive(Default)]
pub struct Context {
    pub v: [u128; 32],
    pub fpcr: u64,
    pub fpsr: u64,
    tls: u64,
}
const _: () = assert!(std::mem::size_of::<Context>() == 544);

pub struct Cpu {
    pub f: Frame,
    pub context: Context,
    pub idle: bool,
    pub memory: Memory,
    decoded: [(u32, super::decode::Op); 256],
    pub reservation: Option<(u64, usize)>,
}

unsafe extern "C" {
    fn arch_counter_value() -> u64;
    fn arch_counter_frequency() -> u64;
    fn wasm_syscall(f: &mut Frame, context: &mut Context) -> bool;
    fn wasm_exception(
        f: &mut Frame,
        context: &mut Context,
        signal: i32,
        address: u64,
        message: *const u8,
        len: usize,
    ) -> bool;
    fn wasm_timer(f: &mut Frame, context: &mut Context, ticks: u32) -> bool;
}
pub fn sx(value: u64, bits: u32) -> u64 {
    (((value << (64 - bits)) as i64) >> (64 - bits)) as u64
}

impl Cpu {
    pub fn new(f: Frame) -> Self {
        Self {
            f,
            context: Context::default(),
            memory: Memory::new(),
            idle: false,
            decoded: [(0, super::decode::Op::Unknown); 256],
            reservation: None,
        }
    }
    pub fn reg(&self, r: u32) -> u64 {
        self.f.x.get(r as usize).copied().unwrap_or(0)
    }
    pub fn set(&mut self, r: u32, value: u64) {
        if let Some(x) = self.f.x.get_mut(r as usize) {
            *x = value;
        }
    }
    pub fn base(&self, r: u32) -> u64 {
        if r == 31 { self.f.sp } else { self.reg(r) }
    }
    pub fn set_base(&mut self, r: u32, value: u64) {
        if r == 31 {
            self.f.sp = value
        } else {
            self.set(r, value)
        }
    }

    pub fn condition(&self, c: u32) -> bool {
        let flags = self.f.pstate >> 28;
        let (n, z, carry, v) = (
            flags & 8 != 0,
            flags & 4 != 0,
            flags & 2 != 0,
            flags & 1 != 0,
        );
        let yes = match c >> 1 {
            0 => z,
            1 => carry,
            2 => n,
            3 => v,
            4 => carry && !z,
            5 => n == v,
            6 => n == v && !z,
            _ => return true,
        };
        yes ^ (c & 1 != 0)
    }

    pub fn exception(&mut self, fault: Fault) {
        let message = fault.to_string();
        let (signal, address) = match fault {
            Fault::Breakpoint => (5, self.f.pc),
            Fault::Instruction(_) => (4, self.f.pc),
            Fault::Alignment(addr) => (7, addr),
            Fault::Memory(_, addr) => (11, addr),
        };
        self.memory.invalidate();
        self.reservation = None;
        self.idle = unsafe {
            wasm_exception(
                &mut self.f,
                &mut self.context,
                signal,
                address,
                message.as_ptr(),
                message.len(),
            )
        };
    }

    pub fn tick(&mut self, ticks: u32) {
        self.memory.invalidate();
        self.reservation = None;
        self.idle = unsafe { wasm_timer(&mut self.f, &mut self.context, ticks) };
    }

    pub fn step(&mut self) -> Result<(), Fault> {
        let pc = self.f.pc;
        let w = self.memory.fetch(pc)?;
        let rd = w & 31;
        let rn = (w >> 5) & 31;
        let mut next = pc.wrapping_add(4);
        use super::decode::Op::*;
        let slot = &mut self.decoded[((pc >> 2) as usize) & 255];
        if slot.0 != w {
            *slot = (w, super::decode::classify(w));
        }
        match slot.1 {
            Branch => {
                if w >> 31 != 0 {
                    self.set(30, next);
                }
                next = pc.wrapping_add(sx((w & 0x03ffffff) as u64, 26) << 2);
            }
            BranchRegister => {
                let target = self.reg(rn);
                if w & 0xfffffc1f == 0xd63f0000 {
                    self.set(30, next);
                }
                next = target;
            }
            Conditional => {
                if self.condition(w & 15) {
                    next = pc.wrapping_add(sx(((w >> 5) & 0x7ffff) as u64, 19) << 2);
                }
            }
            CompareBranch => {
                let value = if w >> 31 != 0 {
                    self.reg(rd)
                } else {
                    self.reg(rd) as u32 as u64
                };
                if (value != 0) == (w >> 24 & 1 != 0) {
                    next = pc.wrapping_add(sx(((w >> 5) & 0x7ffff) as u64, 19) << 2);
                }
            }
            TestBranch => {
                let bit = (w >> 31) * 32 + ((w >> 19) & 31);
                if (self.reg(rd) >> bit) & 1 == ((w >> 24) & 1) as u64 {
                    next = pc.wrapping_add(sx(((w >> 5) & 0x3fff) as u64, 14) << 2);
                }
            }
            Address => {
                let imm = sx((((w >> 5) & 0x7ffff) * 4 + ((w >> 29) & 3)) as u64, 21);
                let addr = if w >> 31 != 0 {
                    (pc & !4095).wrapping_add(imm << 12)
                } else {
                    pc.wrapping_add(imm)
                };
                self.set(rd, addr);
            }
            Breakpoint => return Err(Fault::Breakpoint),
            Syscall => {
                self.memory.invalidate();
                self.reservation = None;
                self.f.pc = next;
                self.idle = unsafe { wasm_syscall(&mut self.f, &mut self.context) };
                return Ok(());
            }
            SystemRegister => match w & 0xffffffe0 {
                0xd53bd040 => self.set(rd, self.context.tls),
                0xd51bd040 => self.context.tls = self.reg(rd),
                0xd53b00e0 => self.set(rd, 16),
                0xd53b0020 => self.set(rd, 0x3444c004),
                0xd53be000 => self.set(rd, unsafe { arch_counter_frequency() }),
                0xd53be020 | 0xd53be040 => self.set(rd, unsafe { arch_counter_value() }),
                0xd53b4200 => self.set(rd, self.f.pstate & 0xf0000000),
                0xd51b4200 => {
                    self.f.pstate = (self.f.pstate & !0xf0000000) | (self.reg(rd) & 0xf0000000)
                }
                0xd53b4400 => self.set(rd, self.context.fpcr),
                0xd53b4420 => self.set(rd, self.context.fpsr),
                0xd51b4400 => self.context.fpcr = self.reg(rd) & 0x07c00000,
                0xd51b4420 => self.context.fpsr = self.reg(rd) & 0xf800009f,
                _ => unreachable!(),
            },
            CacheMaintenance => {
                self.memory.load(self.reg(rd), 1)?;
            }
            Crc => self.crc(w)?,
            Crypto => self.crypto(w)?,
            Float => self.float(w)?,
            NeonInteger(op) => self.neon_integer(op, w)?,
            VectorFloat(op) => self.neon_float(w, op)?,
            Vector => {
                self.simd(w)?;
            }
            ClearExclusive => {
                self.reservation = None;
            }
            Hint => {
                // NOP and the architecturally compatible BTI c hint.
            }
            Transfer => {
                self.transfer(w)?;
            }
            Integer(op) => self.alu(op, w)?,
            Unknown => return Err(Fault::Instruction(w)),
        }
        self.f.pc = next;
        Ok(())
    }
}
