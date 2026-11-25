use super::{Block, I, at};
use crate::cpu::{Context, Cpu};
use std::mem::offset_of;

pub(super) fn vector_offset(reg: u32) -> u64 {
    (offset_of!(Cpu, context) + offset_of!(Context, v)) as u64 + u64::from(reg) * 16
}

fn move_registers(w: u32) -> Option<(u32, u32)> {
    let src = (w >> 5) & 31;
    (w & 0xffe0fc00 == 0x4ea01c00 && src == (w >> 16) & 31).then_some((w & 31, src))
}

impl Block {
    fn vector_move(&mut self, dst: u32, src: u32) {
        self.body.extend([
            I::LocalGet(0),
            I::LocalGet(0),
            I::V128Load(at(vector_offset(src))),
            I::V128Store(at(vector_offset(dst))),
        ]);
    }

    fn sigma(&mut self, value: u32, rotations: [i32; 3]) {
        for (i, shift) in rotations.into_iter().enumerate() {
            self.body
                .extend([I::LocalGet(value), I::I32Const(shift), I::I32Rotr]);
            if i != 0 {
                self.body.push(I::I32Xor);
            }
        }
    }

    fn sha_rounds(&mut self, low: u32, high: u32, keys: u32, write_low: bool, write_high: bool) {
        let lane = |reg, i| wasm_encoder::MemArg {
            align: 2,
            ..at(vector_offset(reg) + i * 4)
        };
        let mut state = [45, 46, 47, 48, 49, 50, 51, 52];
        for (i, &local) in state.iter().enumerate() {
            let reg = if i < 4 { low } else { high };
            self.body.extend([
                I::LocalGet(0),
                I::I32Load(lane(reg, i as u64 % 4)),
                I::LocalSet(local),
            ]);
        }
        for round in 0..4 {
            let [a, b, c, d, e, f, g, h] = state;
            self.sigma(e, [6, 11, 25]);
            self.body.extend([
                I::LocalGet(e),
                I::LocalGet(f),
                I::LocalGet(g),
                I::I32Xor,
                I::I32And,
                I::LocalGet(g),
                I::I32Xor,
                I::I32Add,
                I::LocalGet(h),
                I::I32Add,
                I::LocalGet(0),
                I::I32Load(lane(keys, round)),
                I::I32Add,
                I::LocalTee(h),
                I::LocalGet(d),
                I::I32Add,
                I::LocalSet(d),
            ]);
            self.sigma(a, [2, 13, 22]);
            self.body.extend([
                I::LocalGet(a),
                I::LocalGet(b),
                I::I32And,
                I::LocalGet(a),
                I::LocalGet(b),
                I::I32Xor,
                I::LocalGet(c),
                I::I32And,
                I::I32Or,
                I::I32Add,
                I::LocalGet(h),
                I::I32Add,
                I::LocalSet(h),
            ]);
            state[..4].rotate_right(1);
            state[4..].rotate_right(1);
            state.swap(0, 4);
        }
        for (i, local) in state.into_iter().enumerate() {
            if (i < 4 && write_low) || (i >= 4 && write_high) {
                let reg = if i < 4 { low } else { high };
                self.body.extend([
                    I::LocalGet(0),
                    I::LocalGet(local),
                    I::I32Store(lane(reg, i as u64 % 4)),
                ]);
            }
        }
    }

    pub(super) fn vector(&mut self, words: &[u32]) -> usize {
        let w = words[0];
        if let Some((tmp, low)) = move_registers(w) {
            self.vector_move(tmp, low);
            if let [_, first, second, ..] = *words {
                let high = (first >> 5) & 31;
                let keys = (first >> 16) & 31;
                if first & 0xffe0fc1f == 0x5e004000 | low
                    && second == 0x5e005000 | (keys << 16) | (tmp << 5) | high
                    && low != high
                    && low != keys
                    && ![low, high, keys].contains(&tmp)
                {
                    self.sha_rounds(low, high, keys, true, true);
                    return 3;
                }
            }
            return 1;
        }
        let (d, n, m) = (w & 31, (w >> 5) & 31, (w >> 16) & 31);
        let op = w & 0xfffffc00;
        match op {
            0x9e660000 | 0x1e260000 | 0x9eae0000 => {
                let offset = vector_offset(n) + if op == 0x9eae0000 { 8 } else { 0 };
                self.body.extend([I::LocalGet(0), I::I64Load(at(offset))]);
                self.set(d, false, op != 0x1e260000);
                return 1;
            }
            0x9e670000 | 0x1e270000 | 0x9eaf0000 => {
                let offset = vector_offset(d);
                self.body.push(I::LocalGet(0));
                self.get(n, false);
                self.narrow(op != 0x1e270000);
                if op == 0x9eaf0000 {
                    self.body.push(I::I64Store(at(offset + 8)));
                } else {
                    self.body.extend([
                        I::I64Store(at(offset)),
                        I::LocalGet(0),
                        I::I64Const(0),
                        I::I64Store(at(offset + 8)),
                    ]);
                }
                return 1;
            }
            _ => {}
        }
        match w & 0xffe0fc00 {
            0x5e004000 => self.sha_rounds(d, n, m, true, false),
            0x5e005000 => self.sha_rounds(n, d, m, false, true),
            _ => return 0,
        }
        1
    }
}
