use super::{BlockType, Function, I, NEXT, RETIRED};
use crate::decode::{Integer, Op, classify};

pub(super) fn counter(words: &[u32]) -> Option<(u32, bool)> {
    let (&branch, body) = words.split_last()?;
    if branch & 0xff00001f != 0x54000001 {
        return None;
    }
    let index = body.iter().rposition(|&w| w & 0x7ffffc00 == 0x71000400)?;
    let sub = body[index];
    let reg = sub & 31;
    if reg == 31 || (sub >> 5) & 31 != reg {
        return None;
    }
    for (i, &w) in body.iter().enumerate() {
        if i == index {
            continue;
        }
        match classify(w) {
            Op::Crypto | Op::NeonInteger(_) => {}
            Op::Integer(
                Integer::MoveWide | Integer::MultiplyAdd | Integer::Bitfield | Integer::Extract,
            ) if w & 31 != reg => {}
            Op::Integer(Integer::Logical | Integer::AddImmediate | Integer::AddShifted)
                if w & 31 != reg && (i < index || w & (1 << 29) == 0) => {}
            _ => return None,
        }
    }
    Some((reg + 4, sub >> 31 != 0))
}

pub(super) fn emit(
    code: &mut Function,
    body: &[I],
    pc: u64,
    count: u32,
    (local, wide): (u32, bool),
) {
    for op in [
        I::LocalGet(1),
        I::I32Const(count as i32),
        I::I32DivU,
        I::LocalSet(1),
        I::LocalGet(local),
        I::I64Const(1),
        I::I64Sub,
    ] {
        code.instruction(&op);
    }
    if !wide {
        code.instruction(&I::I32WrapI64)
            .instruction(&I::I64ExtendI32U);
    }
    for op in [
        I::LocalGet(1),
        I::I64ExtendI32U,
        I::I64LtU,
        I::If(BlockType::Empty),
        I::LocalGet(local),
        I::I32WrapI64,
        I::LocalSet(1),
        I::End,
        I::LocalGet(1),
        I::I32Const(count as i32),
        I::I32Mul,
        I::LocalSet(RETIRED),
    ] {
        code.instruction(&op);
    }
    if body.len() < 48 {
        code.instruction(&I::Block(BlockType::Empty))
            .instruction(&I::Loop(BlockType::Empty));
        for op in [I::LocalGet(1), I::I32Const(4), I::I32LtU, I::BrIf(1)] {
            code.instruction(&op);
        }
        for _ in 0..4 {
            for op in body {
                code.instruction(op);
            }
        }
        for op in [
            I::LocalGet(1),
            I::I32Const(4),
            I::I32Sub,
            I::LocalSet(1),
            I::Br(0),
            I::End,
            I::End,
        ] {
            code.instruction(&op);
        }
    }
    code.instruction(&I::Block(BlockType::Empty))
        .instruction(&I::LocalGet(1))
        .instruction(&I::I32Eqz)
        .instruction(&I::BrIf(0))
        .instruction(&I::Loop(BlockType::Empty));
    for op in body {
        code.instruction(op);
    }
    for op in [
        I::LocalGet(1),
        I::I32Const(1),
        I::I32Sub,
        I::LocalTee(1),
        I::BrIf(0),
        I::End,
        I::End,
        I::I64Const((pc + u64::from(count) * 4) as i64),
        I::I64Const(pc as i64),
        I::LocalGet(local),
        I::I64Eqz,
        I::Select,
        I::LocalSet(NEXT),
    ] {
        code.instruction(&op);
    }
}
