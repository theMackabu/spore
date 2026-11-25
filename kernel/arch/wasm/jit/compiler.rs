use super::super::{
    cpu::sx,
    decode::{self, Op},
};
use wasm_encoder::{
    BlockType, CodeSection, EntityType, ExportKind, ExportSection, Function, FunctionSection,
    ImportSection, Instruction as I, MemArg, MemoryType, Module, TypeSection, ValType,
};

mod branch;
mod counted;
mod flags;
mod integer;
mod memory;
mod vector;

const FLAGS: u32 = 36;
const LEFT: u32 = 37;
const RIGHT: u32 = 38;
const VALUE: u32 = 39;
const NEXT: u32 = 40;
const RETIRED: u32 = 42;

struct Block {
    body: Vec<I<'static>>,
    regs: u32,
    side_exits: Vec<(usize, u32)>,
    dirty: u32,
    pending: Option<(u32, bool, bool)>,
    guarded: bool,
    flags_read: bool,
    flags_written: bool,
}
impl Block {
    fn get(&mut self, reg: u32, sp: bool) {
        if reg == 31 && !sp {
            self.body.push(I::I64Const(0));
        } else {
            self.regs |= (1 << reg) & !self.dirty;
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
    fn address(&mut self, pc: u64, w: u32) -> bool {
        use crate::cpu::{Context, Cpu};
        use std::mem::offset_of;
        let field = match w & !31 {
            0xd53bd040 => Some(offset_of!(Context, tls)),
            0xd53b4400 => Some(offset_of!(Context, fpcr)),
            0xd53b4420 => Some(offset_of!(Context, fpsr)),
            _ => None,
        };
        if let Some(field) = field {
            let offset = offset_of!(Cpu, context) + field;
            self.body
                .extend([I::LocalGet(0), I::I64Load(at(offset as u64))]);
            self.set(w & 31, false, true);
            return true;
        }
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
}

fn at(offset: u64) -> MemArg {
    MemArg {
        offset,
        align: 3,
        target: wasm_encoder::MemArgTarget::Memory(0),
        ordering: None,
    }
}

pub struct Trace {
    pub module: Vec<u8>,
    pub spans: Vec<(u64, Vec<u32>)>,
    pub count: u32,
}

pub fn compile(
    pc: u64,
    memory: &mut crate::memory::Memory,
    branches: &[u64; 4096],
) -> Option<Trace> {
    let mut block = Block {
        body: Vec::new(),
        regs: 0,
        side_exits: Vec::new(),
        dirty: 0,
        pending: None,
        guarded: false,
        flags_read: false,
        flags_written: false,
    };
    let mut count = 0;
    let mut target = None;
    let mut branch_at = 0;
    let mut here = pc;
    let mut spans: Vec<(u64, Vec<u32>)> = Vec::new();
    let mut visited = Vec::new();
    let mut calls = Vec::new();
    while count < 128 {
        if visited.contains(&here) {
            block
                .body
                .extend([I::I64Const(here as i64), I::LocalSet(NEXT)]);
            target = Some(here);
            break;
        }
        let Some(words) = memory.trace_words(here) else {
            break;
        };
        visited.push(here);
        let w = words[0];
        let mut executed = block.vector(words);
        if executed == 0 && matches!(decode::classify(w), Op::Transfer) {
            executed = block.transfers(here, count, words);
        }
        if w & 0x7ffffc00 == 0x5ac00000
            && words.get(1) == Some(&((w & 0x80000000) | 0x5ac01000 | ((w & 31) << 5) | (w & 31)))
        {
            block.get((w >> 5) & 31, false);
            if w >> 31 != 0 {
                block.body.push(I::I64Ctz);
            } else {
                block
                    .body
                    .extend([I::I32WrapI64, I::I32Ctz, I::I64ExtendI32U]);
            }
            block.set(w & 31, false, true);
            executed = 2;
        }
        let mut next = here + 4;
        let mut ends = false;
        if executed == 0 {
            executed = 1;
            if !block.integer(w) && !block.address(here, w) {
                let op = decode::classify(w);
                let conditional = matches!(
                    op,
                    Op::Conditional | Op::CompareBranch | Op::TestBranch | Op::BranchRegister
                );
                let mut predicted = branches[(here >> 2) as usize & 4095];
                if w == 0xd65f03c0
                    && let Some(return_pc) = calls.pop()
                {
                    predicted = return_pc;
                }
                if matches!(op, Op::Branch) && w >> 31 != 0 || w & 0xfffffc1f == 0xd63f0000 {
                    calls.push(next);
                }
                let self_branch = matches!(op, Op::Conditional | Op::CompareBranch)
                    && here.wrapping_add(sx(u64::from((w >> 5) & 0x7ffff), 19) << 2) == pc;
                if conditional && !self_branch {
                    block.guarded = true;
                }
                branch_at = block.body.len();
                let Some(dest) = block.branch(here, w) else {
                    break;
                };
                target = Some(dest);
                if matches!(op, Op::Syscall) {
                    target = Some(u64::MAX);
                    block.body.extend([I::I32Const(i32::MIN), I::LocalSet(3)]);
                    ends = true;
                } else {
                    next = if !conditional || self_branch {
                        dest
                    } else if predicted == dest
                        || matches!(op, Op::BranchRegister)
                        || predicted == next
                    {
                        predicted
                    } else {
                        next
                    };
                    if next == pc || next == 0 || next & 3 != 0 {
                        ends = true;
                        if next == pc {
                            target = Some(pc);
                        }
                    } else {
                        if conditional {
                            block.body.extend([
                                I::LocalGet(NEXT),
                                I::I64Const(next as i64),
                                I::I64Ne,
                                I::If(BlockType::Empty),
                            ]);
                            block.side_exit(count + 1);
                        }
                        target = None;
                    }
                }
            }
        } else {
            next = here + executed as u64 * 4;
        }
        if let Some((start, code)) = spans.last_mut()
            && *start + code.len() as u64 * 4 == here
            && *start >> 12 == here >> 12
        {
            code.extend_from_slice(&words[..executed]);
        } else {
            spans.push((here, words[..executed].to_vec()));
        }
        count += executed;
        here = next;
        if ends {
            break;
        }
    }
    if count == 0 {
        return None;
    }
    if target.is_none() {
        block
            .body
            .extend([I::I64Const(here as i64), I::LocalSet(NEXT)]);
    }
    let loops = target == Some(pc);
    let mut writebacks = Vec::new();
    if !loops {
        for &(at, mask) in &block.side_exits {
            if writebacks.last() != Some(&mask) {
                writebacks.push(mask);
            }
            block.body[at] = I::Br((writebacks.len() - 1) as u32);
        }
    }
    if writebacks.last() != Some(&block.dirty) {
        writebacks.push(block.dirty);
    }
    if !loops {
        for &(at, _) in &block.side_exits {
            let I::Br(index) = block.body[at] else {
                unreachable!()
            };
            block.body[at] = I::Br(2 + writebacks.len() as u32 - 1 - index);
        }
    }
    let mut code = Function::new([(38, ValType::I64), (11, ValType::I32)]);
    for r in 0..32 {
        if (block.regs
            | if loops && block.guarded {
                block.dirty
            } else {
                0
            })
            & (1 << r)
            != 0
        {
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
    for _ in &writebacks {
        code.instruction(&I::Block(BlockType::Empty));
    }
    let counter = if loops && !block.guarded && !block.flags_read && spans.len() == 1 {
        counted::counter(&spans[0].1)
    } else {
        None
    };
    if let Some(counter) = counter {
        counted::emit(
            &mut code,
            &block.body[..branch_at],
            pc,
            count as u32,
            counter,
        );
        block.body.clear();
        block.pack_flags();
        for op in &block.body {
            code.instruction(op);
        }
    } else {
        code.instruction(&I::Loop(BlockType::Empty));
        for i in &block.body {
            code.instruction(i);
        }
        code.instruction(&I::LocalGet(RETIRED))
            .instruction(&I::I32Const(count as i32))
            .instruction(&I::I32Add)
            .instruction(&I::LocalSet(RETIRED));
        block.body.clear();
        block.pack_flags();
        let exit_flags = std::mem::take(&mut block.body);
        if block.flags_read && target == Some(pc) {
            for op in &exit_flags {
                code.instruction(op);
            }
        }
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
        if !block.flags_read || target != Some(pc) {
            for op in &exit_flags {
                code.instruction(op);
            }
        }
        code.instruction(&I::End);
    }
    for (index, &mask) in writebacks.iter().enumerate().rev() {
        code.instruction(&I::End);
        let mut regs = mask & !index.checked_sub(1).map_or(0, |prev| writebacks[prev]);
        while regs != 0 {
            let r = regs.trailing_zeros();
            regs &= regs - 1;
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
        .instruction(&I::LocalGet(3))
        .instruction(&I::I32Or)
        .instruction(&I::End);
    let mut module = Module::new();
    let mut types = TypeSection::new();
    types.ty().function([ValType::I32; 4], [ValType::I32]);
    types.ty().function([ValType::I64; 2], [ValType::I64]);
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
    imports.import("spore", "umulh", EntityType::Function(1));
    module.section(&imports);
    let mut functions = FunctionSection::new();
    functions.function(0);
    module.section(&functions);
    let mut exports = ExportSection::new();
    exports.export("run", ExportKind::Func, 1);
    module.section(&exports);
    let mut codes = CodeSection::new();
    codes.function(&code);
    module.section(&codes);
    Some(Trace {
        module: module.finish(),
        spans,
        count: count as u32,
    })
}
