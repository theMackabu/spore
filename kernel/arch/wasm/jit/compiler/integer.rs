use super::{Block, BlockType, FLAGS, I, LEFT, RIGHT, VALUE};
use crate::decode::{self, Integer, Op};
impl Block {
    pub(super) fn integer(&mut self, w: u32) -> bool {
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
        let extended = matches!(kind, Integer::AddExtended);
        let arithmetic = extended || immediate || matches!(kind, Integer::AddShifted);
        let log = matches!(kind, Integer::Logical) && w & 0x1f000000 == 0x0a000000;
        let logical_imm = matches!(kind, Integer::Logical) && !log;
        if arithmetic || log || logical_imm {
            let shift = (w >> 10) & 63;
            let mode = (w >> 22) & 3;
            if extended && (shift & 7) > 4
                || !extended
                    && !immediate
                    && !logical_imm
                    && (shift >= bits || (arithmetic && mode == 3))
            {
                return false;
            }
            let flags = if log || logical_imm {
                op == 3
            } else {
                op & 1 != 0
            };
            let operand = if logical_imm {
                let Some(value) =
                    crate::alu::logical_mask((w >> 22) & 1, (w >> 16) & 63, shift, bits)
                else {
                    return false;
                };
                Some(value)
            } else {
                None
            };
            self.get(n, immediate || extended);
            if flags {
                self.narrow(wide);
                self.body.push(I::LocalTee(LEFT));
            }
            if let Some(mask) = operand {
                self.body.push(I::I64Const(mask as i64));
            } else if immediate {
                let imm = ((w >> 10) & 4095) << (12 * ((w >> 22) & 1));
                self.body.push(I::I64Const(i64::from(imm)));
            } else if extended {
                self.get(m, false);
                let extend = (w >> 13) & 7;
                let width = 8 << (extend & 3);
                if width < 64 {
                    if extend & 4 == 0 {
                        self.body
                            .extend([I::I64Const((1i64 << width) - 1), I::I64And]);
                    } else {
                        self.body.push(match width {
                            8 => I::I64Extend8S,
                            16 => I::I64Extend16S,
                            _ => I::I64Extend32S,
                        });
                    }
                }
                self.shift(shift & 7, 0, true);
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
                self.flags_written = true;
                self.pending = Some((bits, arithmetic, sub));
            }
            self.set(d, (immediate || extended || logical_imm) && !flags, wide);
        } else if matches!(kind, Integer::Extract) {
            let shift = (w >> 10) & 63;
            if shift >= bits || (w >> 22) & 1 != w >> 31 {
                return false;
            }
            self.get(m, false);
            if n == m {
                self.shift(shift, 3, wide);
            } else if shift != 0 {
                self.shift(shift, 1, wide);
                self.get(n, false);
                self.shift(bits - shift, 0, wide);
                self.body.push(I::I64Or);
            }
            self.set(d, false, wide);
        } else if matches!(kind, Integer::VariableShift) {
            self.get(n, false);
            if !wide {
                self.body.push(I::I32WrapI64);
            }
            self.get(m, false);
            let mode = (w >> 10) & 3;
            if wide {
                self.body
                    .push([I::I64Shl, I::I64ShrU, I::I64ShrS, I::I64Rotr][mode as usize].clone());
            } else {
                self.body.extend([
                    I::I32WrapI64,
                    [I::I32Shl, I::I32ShrU, I::I32ShrS, I::I32Rotr][mode as usize].clone(),
                    I::I64ExtendI32U,
                ]);
            }
            self.set(d, false, true);
        } else if matches!(kind, Integer::ReverseBytes) {
            let width = 1 << ((w >> 10) & 3);
            let mask = std::array::from_fn(|i| (i ^ (width - 1)) as u8);
            self.get(n, false);
            self.body.extend([
                I::I64x2Splat,
                I::V128Const(i128::from_le_bytes(mask)),
                I::I8x16Swizzle,
                I::I64x2ExtractLane(0),
            ]);
            self.set(d, false, wide);
        } else if matches!(kind, Integer::LeadingZeros) {
            self.get(n, false);
            if wide {
                self.body.push(I::I64Clz);
            } else {
                self.body
                    .extend([I::I32WrapI64, I::I32Clz, I::I64ExtendI32U]);
            }
            self.set(d, false, true);
        } else if matches!(kind, Integer::MultiplyHigh) {
            self.get(n, false);
            self.get(m, false);
            self.body.push(I::Call(0));
            if w & (1 << 23) == 0 {
                self.get(n, false);
                self.shift(63, 2, true);
                self.get(m, false);
                self.body.extend([I::I64And, I::I64Sub]);
                self.get(m, false);
                self.shift(63, 2, true);
                self.get(n, false);
                self.body.extend([I::I64And, I::I64Sub]);
            }
            self.set(d, false, true);
        } else if matches!(kind, Integer::Bitfield) {
            let r = (w >> 16) & 63;
            let s = (w >> 10) & 63;
            if r >= bits || s >= bits || (w >> 22) & 1 != w >> 31 || op == 3 {
                return false;
            }
            let (lsb, width) = if s >= r {
                (0, s - r + 1)
            } else {
                (bits - r, s + 1)
            };
            let mask = u64::MAX >> (64 - width);
            self.get(n, false);
            if s >= r {
                self.shift(r, 1, true);
            }
            self.body.extend([I::I64Const(mask as i64), I::I64And]);
            self.shift(lsb, 0, true);
            if op == 0 {
                self.shift(64 - width - lsb, 0, true);
                self.shift(64 - width - lsb, 2, true);
            } else if op == 1 {
                self.get(d, false);
                self.body
                    .extend([I::I64Const(!(mask << lsb) as i64), I::I64And, I::I64Or]);
            }
            self.set(d, false, wide);
        } else if matches!(kind, Integer::Select) {
            self.get(n, false);
            self.get(m, false);
            if w & (1 << 30) != 0 {
                self.body.extend([I::I64Const(-1), I::I64Xor]);
            }
            if w & (1 << 10) != 0 {
                self.body.extend([I::I64Const(1), I::I64Add]);
            }
            self.condition((w >> 12) & 15);
            self.body.push(I::Select);
            self.set(d, false, wide);
        } else if matches!(kind, Integer::Compare) {
            self.condition((w >> 12) & 15);
            self.body.push(I::If(BlockType::Empty));
            self.get(n, false);
            self.narrow(wide);
            self.body.push(I::LocalTee(LEFT));
            if w & 0x800 != 0 {
                self.body.push(I::I64Const(i64::from(m)));
            } else {
                self.get(m, false);
                self.narrow(wide);
            }
            self.body
                .extend([I::LocalTee(RIGHT), if sub { I::I64Sub } else { I::I64Add }]);
            self.narrow(wide);
            self.body.push(I::LocalSet(VALUE));
            self.flags(bits, true, sub);
            self.body.extend([
                I::Else,
                I::I64Const(i64::from(w & 15)),
                I::LocalSet(FLAGS),
                I::End,
            ]);
            self.pending = None;
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
}
