use super::{Block, I, NEXT};
use crate::{
    cpu::sx,
    decode::{self, Op},
};
impl Block {
    pub(super) fn branch(&mut self, pc: u64, w: u32) -> Option<u64> {
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
