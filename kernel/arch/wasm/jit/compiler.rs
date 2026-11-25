use super::super::{
    cpu::sx,
    decode::{self, Integer, Op},
};
use wasm_encoder::{
    BlockType, CodeSection, EntityType, ExportKind, ExportSection, Function, FunctionSection,
    ImportSection, Instruction as I, MemArg, MemoryType, Module, TypeSection, ValType,
};

mod memory;

const FLAGS: u32 = 36;
const LEFT: u32 = 37;
const RIGHT: u32 = 38;
const VALUE: u32 = 39;
const NEXT: u32 = 40;
const RETIRED: u32 = 42;

struct Block {
    body: Vec<I<'static>>,
    regs: u32,
    dirty: u32,
    flag_ops: std::ops::Range<usize>,
    deferred_flags: Vec<I<'static>>,
    guarded: bool,
    flags_read: bool,
    flags_written: bool,
}
impl Block {
    fn get(&mut self, reg: u32, sp: bool) {
        if reg == 31 && !sp {
            self.body.push(I::I64Const(0));
        } else {
            self.regs |= 1 << reg;
            self.body.push(I::LocalGet(reg + 4));
        }
    }
    fn set(&mut self, reg: u32, sp: bool, wide: bool) {
        if reg == 31 && !sp {
            self.body.push(I::Drop);
            return;
        }
        self.narrow(wide);
        self.dirty |= 1 << reg;
        self.body.push(I::LocalSet(reg + 4));
    }
    fn narrow(&mut self, wide: bool) {
        if !wide {
            self.body.extend([I::I32WrapI64, I::I64ExtendI32U]);
        }
    }
    fn shift(&mut self, amount: u32, kind: u32, wide: bool) {
        if amount == 0 {
            return;
        }
        if !wide {
            self.body.extend([
                I::I32WrapI64,
                I::I32Const(amount as i32),
                match kind {
                    0 => I::I32Shl,
                    1 => I::I32ShrU,
                    2 => I::I32ShrS,
                    _ => I::I32Rotr,
                },
                I::I64ExtendI32U,
            ]);
        } else {
            self.body.extend([
                I::I64Const(amount as i64),
                match kind {
                    0 => I::I64Shl,
                    1 => I::I64ShrU,
                    2 => I::I64ShrS,
                    _ => I::I64Rotr,
                },
            ]);
        }
    }
    fn flags(&mut self, bits: u32, arithmetic: bool, sub: bool) {
        self.flags_written = true;
        self.body.extend([
            I::LocalGet(VALUE),
            I::I64Const(i64::from(bits - 1)),
            I::I64ShrU,
            I::I64Const(3),
            I::I64Shl,
            I::LocalGet(VALUE),
            I::I64Eqz,
            I::I64ExtendI32U,
            I::I64Const(2),
            I::I64Shl,
            I::I64Or,
        ]);
        if arithmetic {
            if sub {
                self.body
                    .extend([I::LocalGet(LEFT), I::LocalGet(RIGHT), I::I64GeU]);
            } else {
                self.body
                    .extend([I::LocalGet(VALUE), I::LocalGet(LEFT), I::I64LtU]);
            }
            self.body.extend([
                I::I64ExtendI32U,
                I::I64Const(1),
                I::I64Shl,
                I::I64Or,
                I::LocalGet(LEFT),
                I::LocalGet(RIGHT),
                I::I64Xor,
            ]);
            if !sub {
                self.body.extend([I::I64Const(-1), I::I64Xor]);
            }
            self.body.extend([
                I::LocalGet(LEFT),
                I::LocalGet(VALUE),
                I::I64Xor,
                I::I64And,
                I::I64Const(i64::from(bits - 1)),
                I::I64ShrU,
                I::I64Const(1),
                I::I64And,
                I::I64Or,
            ]);
        }
        self.body.push(I::LocalSet(FLAGS));
    }

    fn address(&mut self, pc: u64, w: u32) -> bool {
        if !matches!(decode::classify(w), Op::Address) {
            return false;
        }
        let imm = sx(u64::from(((w >> 5) & 0x7ffff) * 4 + ((w >> 29) & 3)), 21);
        let value = if w >> 31 != 0 {
            (pc & !4095).wrapping_add(imm << 12)
        } else {
            pc.wrapping_add(imm)
        };
        self.body.push(I::I64Const(value as i64));
        self.set(w & 31, false, true);
        true
    }

    fn integer(&mut self, w: u32) -> bool {
        let Op::Integer(kind) = decode::classify(w) else {
            return false;
        };
        let d = w & 31;
        let n = (w >> 5) & 31;
        let m = (w >> 16) & 31;
        let wide = w >> 31 != 0;
        let bits = if wide { 64 } else { 32 };
        let op = (w >> 29) & 3;
        let sub = op & 2 != 0;
        let immediate = matches!(kind, Integer::AddImmediate);
        let arithmetic = immediate || matches!(kind, Integer::AddShifted);
        let log = matches!(kind, Integer::Logical) && w & 0x1f000000 == 0x0a000000;
        let logical_imm = matches!(kind, Integer::Logical) && !log;
        if arithmetic || log || logical_imm {
            let shift = (w >> 10) & 63;
            let mode = (w >> 22) & 3;
            if !immediate && !logical_imm && (shift >= bits || (arithmetic && mode == 3)) {
                return false;
            }
            let flags = if log || logical_imm {
                op == 3
            } else {
                op & 1 != 0
            };
            let operand = if logical_imm {
                let Some(value) =
                    super::super::alu::logical_mask((w >> 22) & 1, (w >> 16) & 63, shift, bits)
                else {
                    return false;
                };
                Some(value)
            } else {
                None
            };
            self.get(n, immediate);
            if flags {
                self.narrow(wide);
                self.body.push(I::LocalTee(LEFT));
            }
            if let Some(mask) = operand {
                self.body.push(I::I64Const(mask as i64));
            } else if immediate {
                let imm = ((w >> 10) & 4095) << (12 * ((w >> 22) & 1));
                self.body.push(I::I64Const(i64::from(imm)));
            } else {
                self.get(m, false);
                self.shift(shift, mode, wide);
                if log && w & (1 << 21) != 0 {
                    self.body.extend([I::I64Const(-1), I::I64Xor]);
                }
            }
            if flags {
                self.narrow(wide);
                self.body.push(I::LocalTee(RIGHT));
            }
            self.body.push(if log || logical_imm {
                match op {
                    1 => I::I64Or,
                    2 => I::I64Xor,
                    _ => I::I64And,
                }
            } else if sub {
                I::I64Sub
            } else {
                I::I64Add
            });
            if flags {
                self.narrow(wide);
                self.body.push(I::LocalTee(VALUE));
                let start = self.body.len();
                self.flags(bits, arithmetic, sub);
                self.flag_ops = start..self.body.len();
            }
            self.set(d, (immediate || logical_imm) && !flags, wide);
        } else if matches!(kind, Integer::MoveWide) {
            let shift = ((w >> 21) & 3) * 16;
            if shift >= bits || op == 1 {
                return false;
            }
            let imm = i64::from((w >> 5) & 65535) << shift;
            if op == 3 {
                self.get(d, false);
                self.body.extend([
                    I::I64Const(!(65535i64 << shift)),
                    I::I64And,
                    I::I64Const(imm),
                    I::I64Or,
                ]);
            } else {
                self.body
                    .push(I::I64Const(if op == 0 { !imm } else { imm }));
            }
            self.set(d, false, wide);
        } else if matches!(kind, Integer::MultiplyAdd) {
            self.get((w >> 10) & 31, false);
            self.get(n, false);
            self.get(m, false);
            self.body.extend([
                I::I64Mul,
                if w & 0x8000 != 0 {
                    I::I64Sub
                } else {
                    I::I64Add
                },
            ]);
            self.set(d, false, wide);
        } else {
            return false;
        }
        true
    }

    fn flag(&mut self, bit: u32) {
        self.body.extend([
            I::LocalGet(FLAGS),
            I::I32WrapI64,
            I::I32Const(bit as i32),
            I::I32ShrU,
            I::I32Const(1),
            I::I32And,
        ]);
    }
    fn condition(&mut self, cond: u32) {
        if cond <= 1 && !self.guarded && !self.flag_ops.is_empty() {
            self.deferred_flags
                .extend(self.body.drain(self.flag_ops.clone()));
            self.body.extend([I::LocalGet(VALUE), I::I64Eqz]);
            if cond == 1 {
                self.body.push(I::I32Eqz);
            }
            return;
        }
        self.flags_read = true;
        match cond >> 1 {
            0 => self.flag(2),
            1 => self.flag(1),
            2 => self.flag(3),
            3 => self.flag(0),
            4 => {
                self.flag(1);
                self.flag(2);
                self.body.extend([I::I32Eqz, I::I32And]);
            }
            5 | 6 => {
                self.flag(3);
                self.flag(0);
                self.body.push(I::I32Eq);
                if cond >> 1 == 6 {
                    self.flag(2);
                    self.body.extend([I::I32Eqz, I::I32And]);
                }
            }
            _ => {
                self.body.push(I::I32Const(1));
                return;
            }
        }
        if cond & 1 != 0 {
            self.body.push(I::I32Eqz);
        }
    }
    fn branch(&mut self, pc: u64, w: u32) -> Option<u64> {
        let next = pc.wrapping_add(4);
        let op = decode::classify(w);
        if matches!(op, Op::Syscall) {
            self.body
                .extend([I::I64Const(next as i64), I::LocalSet(NEXT)]);
            return Some(next);
        }
        if matches!(op, Op::BranchRegister) {
            self.get((w >> 5) & 31, false);
            self.body.push(I::LocalSet(NEXT));
            if w & 0xfffffc1f == 0xd63f0000 {
                self.body.push(I::I64Const(next as i64));
                self.set(30, false, true);
            }
            return Some(u64::MAX);
        }
        let target = match op {
            Op::Branch => {
                if w >> 31 != 0 {
                    self.body.push(I::I64Const(next as i64));
                    self.set(30, false, true);
                }
                pc.wrapping_add(sx(u64::from(w & 0x03ffffff), 26) << 2)
            }
            Op::CompareBranch | Op::Conditional => {
                pc.wrapping_add(sx(u64::from((w >> 5) & 0x7ffff), 19) << 2)
            }
            Op::TestBranch => pc.wrapping_add(sx(u64::from((w >> 5) & 0x3fff), 14) << 2),
            _ => return None,
        };
        self.body.push(I::I64Const(target as i64));
        if !matches!(op, Op::Branch) {
            self.body.push(I::I64Const(next as i64));
            match op {
                Op::Conditional => self.condition(w & 15),
                Op::CompareBranch => {
                    self.get(w & 31, false);
                    self.narrow(w >> 31 != 0);
                    self.body.push(I::I64Eqz);
                    if w & (1 << 24) != 0 {
                        self.body.push(I::I32Eqz);
                    }
                }
                _ => {
                    self.get(w & 31, false);
                    self.body.extend([
                        I::I64Const(i64::from((w >> 31) * 32 + ((w >> 19) & 31))),
                        I::I64ShrU,
                        I::I32WrapI64,
                        I::I32Const(1),
                        I::I32And,
                    ]);
                    if w & (1 << 24) == 0 {
                        self.body.push(I::I32Eqz);
                    }
                }
            }
            self.body.push(I::Select);
        }
        self.body.push(I::LocalSet(NEXT));
        Some(target)
    }
}

fn at(offset: u64) -> MemArg {
    MemArg {
        offset,
        align: 3,
        target: wasm_encoder::MemArgTarget::Memory(0),
        ordering: None,
    }
}

pub fn compile(pc: u64, words: &[u32]) -> Option<(Vec<u8>, usize)> {
    let mut block = Block {
        body: Vec::new(),
        regs: 0,
        dirty: 0,
        flag_ops: 0..0,
        deferred_flags: Vec::new(),
        guarded: false,
        flags_read: false,
        flags_written: false,
    };
    let mut count = 0;
    let mut target = None;
    for &w in words {
        let here = pc + count as u64 * 4;
        if !block.integer(w) && !block.memory(here, count, w) && !block.address(here, w) {
            target = block.branch(here, w);
            if target.is_some() {
                count += 1;
            }
            break;
        }
        count += 1;
    }
    if count < 2 || (count < 4 && target != Some(pc)) {
        return None;
    }
    if target.is_none() {
        block.body.extend([
            I::I64Const((pc + count as u64 * 4) as i64),
            I::LocalSet(NEXT),
        ]);
    }
    let mut code = Function::new([(38, ValType::I64), (3, ValType::I32)]);
    for r in 0..32 {
        if (block.regs | if block.guarded { block.dirty } else { 0 }) & (1 << r) != 0 {
            code.instruction(&I::LocalGet(0))
                .instruction(&I::I64Load(at(u64::from(r) * 8)))
                .instruction(&I::LocalSet(4 + r));
        }
    }
    if block.flags_read || block.guarded && block.flags_written {
        code.instruction(&I::LocalGet(0))
            .instruction(&I::I64Load(at(264)))
            .instruction(&I::I64Const(28))
            .instruction(&I::I64ShrU)
            .instruction(&I::LocalSet(FLAGS));
    }
    code.instruction(&I::Block(BlockType::Empty));
    code.instruction(&I::Loop(BlockType::Empty));
    for i in &block.body {
        code.instruction(i);
    }
    code.instruction(&I::LocalGet(RETIRED))
        .instruction(&I::I32Const(count as i32))
        .instruction(&I::I32Add)
        .instruction(&I::LocalSet(RETIRED));
    if target == Some(pc) {
        code.instruction(&I::LocalGet(NEXT))
            .instruction(&I::I64Const(pc as i64))
            .instruction(&I::I64Eq)
            .instruction(&I::LocalGet(1))
            .instruction(&I::LocalGet(RETIRED))
            .instruction(&I::I32Sub)
            .instruction(&I::I32Const(count as i32))
            .instruction(&I::I32GeU)
            .instruction(&I::I32And)
            .instruction(&I::BrIf(0));
    }
    code.instruction(&I::End).instruction(&I::End);
    for op in &block.deferred_flags {
        code.instruction(op);
    }
    for r in 0..32 {
        if block.dirty & (1 << r) != 0 {
            code.instruction(&I::LocalGet(0))
                .instruction(&I::LocalGet(4 + r))
                .instruction(&I::I64Store(at(u64::from(r) * 8)));
        }
    }
    if block.flags_written {
        code.instruction(&I::LocalGet(0))
            .instruction(&I::LocalGet(0))
            .instruction(&I::I64Load(at(264)))
            .instruction(&I::I64Const(!(0xf0000000u64) as i64))
            .instruction(&I::I64And)
            .instruction(&I::LocalGet(FLAGS))
            .instruction(&I::I64Const(28))
            .instruction(&I::I64Shl)
            .instruction(&I::I64Or)
            .instruction(&I::I64Store(at(264)));
    }
    code.instruction(&I::LocalGet(0))
        .instruction(&I::LocalGet(NEXT))
        .instruction(&I::I64Store(at(256)))
        .instruction(&I::LocalGet(RETIRED))
        .instruction(&I::End);
    let mut module = Module::new();
    let mut types = TypeSection::new();
    types.ty().function([ValType::I32; 4], [ValType::I32]);
    module.section(&types);
    let mut imports = ImportSection::new();
    imports.import(
        "spore",
        "memory",
        EntityType::Memory(MemoryType {
            minimum: 0,
            maximum: None,
            memory64: false,
            shared: false,
            page_size_log2: None,
        }),
    );
    module.section(&imports);
    let mut functions = FunctionSection::new();
    functions.function(0);
    module.section(&functions);
    let mut exports = ExportSection::new();
    exports.export("run", ExportKind::Func, 0);
    module.section(&exports);
    let mut codes = CodeSection::new();
    codes.function(&code);
    module.section(&codes);
    Some((module.finish(), count))
}
