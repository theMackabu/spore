use super::{Block, BlockType, I, NEXT, at};
use crate::{cpu::sx, memory::SLOTS};

const ADDR: u32 = 41;
const SLOT: u32 = 43;
const HOST: u32 = 44;

#[derive(Clone, Copy)]
struct Transfer {
    size: u32,
    load: bool,
    first: u32,
    second: Option<u32>,
    base: u32,
    displacement: i64,
    update: bool,
    post: bool,
    index: Option<(u32, u32, u32)>,
    vector: bool,
}
impl Transfer {
    fn bytes(&self) -> i64 {
        i64::from(self.size) * (1 + i64::from(self.second.is_some()))
    }
    fn decode(w: u32) -> Option<Self> {
        let vector = w & (1 << 26) != 0;
        let scale = w >> 30;
        let base = (w >> 5) & 31;
        let first = w & 31;
        if w & 0x3a000000 == 0x28000000 && (matches!(scale, 0 | 2) || vector && scale == 1) {
            let size = if vector {
                4 << scale
            } else if scale == 0 {
                4
            } else {
                8
            };
            let mode = (w >> 23) & 3;
            let second = (w >> 10) & 31;
            if !vector && mode & 1 != 0 && base != 31 && (base == first || base == second) {
                return None;
            }
            return Some(Self {
                size,
                load: w & (1 << 22) != 0,
                first,
                second: Some(second),
                base,
                displacement: (sx(u64::from((w >> 15) & 127), 7) as i64) * i64::from(size),
                update: mode & 1 != 0,
                post: mode == 1,
                index: None,
                vector,
            });
        }
        let op = (w >> 22) & 3;
        if w & 0x3a000000 != 0x38000000 || op > 1 && !(vector && scale == 0) {
            return None;
        }
        let scale = if op > 1 { 4 } else { scale };
        let size = 1 << scale;
        let mut index = None;
        let (displacement, mode) = if w & (1 << 24) != 0 {
            (i64::from((w >> 10) & 4095) * i64::from(size), 0)
        } else if w & (1 << 21) == 0 {
            (sx(u64::from((w >> 12) & 511), 9) as i64, (w >> 10) & 3)
        } else if (w >> 10) & 3 == 2 && w & (1 << 14) != 0 {
            index = Some((
                (w >> 16) & 31,
                (w >> 13) & 7,
                if w & (1 << 12) == 0 { 0 } else { scale },
            ));
            (0, 0)
        } else {
            return None;
        };
        if vector && mode == 2 || !vector && mode & 1 != 0 && base == first && base != 31 {
            return None;
        }
        Some(Self {
            size,
            load: w & (1 << 22) != 0,
            first,
            second: None,
            base,
            displacement,
            update: mode & 1 != 0,
            post: mode == 1,
            index,
            vector,
        })
    }
}

impl Block {
    pub(super) fn transfers(&mut self, pc: u64, retired: usize, words: &[u32]) -> usize {
        let Some(first) = Transfer::decode(words[0]) else {
            return 0;
        };
        let offset = if first.post { 0 } else { first.displacement };
        let mut start = offset;
        let mut end = start + first.bytes();
        let mut count = 1;
        if first.base == 31 && !first.update && first.index.is_none() {
            for &word in words.iter().skip(1).take(15) {
                let Some(t) = Transfer::decode(word) else {
                    break;
                };
                if t.base != 31 || t.update || t.index.is_some() || t.load != first.load {
                    break;
                }
                let low = start.min(t.displacement);
                let high = end.max(t.displacement + t.bytes());
                if high - low > 4096 {
                    break;
                }
                (start, end) = (low, high);
                count += 1;
            }
        }
        self.guard_transfer(&first, pc, retired, start, (end - start) as u32);
        self.transfer_data(&first, HOST, (offset - start) as u64);
        for &word in &words[1..count] {
            let t = Transfer::decode(word).unwrap();
            self.transfer_data(&t, HOST, (t.displacement - start) as u64);
        }
        if first.update {
            self.get(first.base, true);
            self.body
                .extend([I::I64Const(first.displacement), I::I64Add]);
            self.set(first.base, true, true);
        }
        count
    }

    fn guard_transfer(&mut self, t: &Transfer, pc: u64, retired: usize, start: i64, bytes: u32) {
        self.guarded = true;
        self.get(t.base, true);
        if let Some((reg, extend, shift)) = t.index {
            self.get(reg, false);
            match extend {
                2 => self.narrow(false),
                6 => self.body.extend([I::I32WrapI64, I::I64ExtendI32S]),
                _ => {}
            }
            self.shift(shift, 0, true);
            self.body.push(I::I64Add);
        }
        if start != 0 {
            self.body.extend([I::I64Const(start), I::I64Add]);
        }
        self.body.push(I::LocalSet(ADDR));
        let bank = if t.load { 0 } else { SLOTS * 16 };
        self.body.extend([
            I::LocalGet(2),
            I::LocalGet(ADDR),
            I::I32WrapI64,
            I::I32Const(12),
            I::I32ShrU,
            I::I32Const(SLOTS as i32 - 1),
            I::I32And,
            I::I32Const(4),
            I::I32Shl,
            I::I32Add,
            I::LocalTee(SLOT),
            I::I64Load(at(bank as u64)),
            I::LocalGet(ADDR),
            I::I64Const(12),
            I::I64ShrU,
            I::I64Ne,
            I::LocalGet(ADDR),
            I::I32WrapI64,
            I::I32Const(4095),
            I::I32And,
            I::I32Const((4096 - bytes) as i32),
            I::I32GtU,
            I::I32Or,
            I::LocalGet(SLOT),
            I::I32Load(wasm_encoder::MemArg {
                align: 2,
                ..at(bank as u64 + 8)
            }),
            I::LocalSet(HOST),
        ]);
        if t.base == 31 {
            self.get(t.base, true);
            self.body
                .extend([I::I32WrapI64, I::I32Const(15), I::I32And, I::I32Or]);
        }
        self.body.extend([
            I::If(BlockType::Empty),
            I::I64Const(pc as i64),
            I::LocalSet(NEXT),
        ]);
        self.side_exit(retired);
        self.body.extend([
            I::LocalGet(HOST),
            I::LocalGet(ADDR),
            I::I32WrapI64,
            I::I32Const(4095),
            I::I32And,
            I::I32Add,
            I::LocalSet(HOST),
        ]);
    }
    fn transfer_data(&mut self, t: &Transfer, host: u32, offset: u64) {
        for (i, r) in std::iter::once(t.first).chain(t.second).enumerate() {
            let arg = wasm_encoder::MemArg {
                align: t.size.ilog2(),
                ..at(offset + (i as u64) * u64::from(t.size))
            };
            if t.vector {
                let register = at(super::vector::vector_offset(r));
                if t.load {
                    self.body.extend([I::LocalGet(0), I::LocalGet(host)]);
                    if t.size == 16 {
                        self.body.extend([I::V128Load(arg), I::V128Store(register)]);
                    } else {
                        self.body.extend([
                            match t.size {
                                1 => I::I64Load8U(arg),
                                2 => I::I64Load16U(arg),
                                4 => I::I64Load32U(arg),
                                _ => I::I64Load(arg),
                            },
                            I::I64Store(register),
                            I::LocalGet(0),
                            I::I64Const(0),
                            I::I64Store(at(register.offset + 8)),
                        ]);
                    }
                } else {
                    self.body.extend([I::LocalGet(host), I::LocalGet(0)]);
                    if t.size == 16 {
                        self.body.extend([I::V128Load(register), I::V128Store(arg)]);
                    } else {
                        self.body.extend([
                            I::I64Load(register),
                            match t.size {
                                1 => I::I64Store8(arg),
                                2 => I::I64Store16(arg),
                                4 => I::I64Store32(arg),
                                _ => I::I64Store(arg),
                            },
                        ]);
                    }
                }
                continue;
            }
            self.body.push(I::LocalGet(host));
            if t.load {
                self.body.push(match t.size {
                    1 => I::I64Load8U(arg),
                    2 => I::I64Load16U(arg),
                    4 => I::I64Load32U(arg),
                    _ => I::I64Load(arg),
                });
                self.set(r, false, true);
            } else {
                self.get(r, false);
                self.body.push(match t.size {
                    1 => I::I64Store8(arg),
                    2 => I::I64Store16(arg),
                    4 => I::I64Store32(arg),
                    _ => I::I64Store(arg),
                });
            }
        }
    }
}
