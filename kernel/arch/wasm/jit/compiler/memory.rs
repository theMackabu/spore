use super::{Block, BlockType, I, NEXT, RETIRED, at};
use crate::{cpu::sx, memory::SLOTS};

const ADDR: u32 = 41;
const SLOT: u32 = 43;
const HOST: u32 = 44;

struct Transfer {
    size: u32,
    load: bool,
    first: u32,
    second: Option<u32>,
    base: u32,
    displacement: i64,
    update: bool,
    post: bool,
}
impl Transfer {
    fn decode(w: u32) -> Option<Self> {
        if w & (1 << 26) != 0 {
            return None;
        }
        let scale = w >> 30;
        let base = (w >> 5) & 31;
        let first = w & 31;
        if w & 0x3a000000 == 0x28000000 && matches!(scale, 0 | 2) {
            let size = if scale == 0 { 4 } else { 8 };
            let mode = (w >> 23) & 3;
            let second = (w >> 10) & 31;
            if mode & 1 != 0 && base != 31 && (base == first || base == second) {
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
            });
        }
        if w & 0x3a000000 != 0x38000000 || (w >> 22) & 3 > 1 {
            return None;
        }
        let size = 1 << scale;
        let (displacement, mode) = if w & (1 << 24) != 0 {
            (i64::from((w >> 10) & 4095) * i64::from(size), 0)
        } else if w & (1 << 21) == 0 {
            (sx(u64::from((w >> 12) & 511), 9) as i64, (w >> 10) & 3)
        } else {
            return None;
        };
        if mode & 1 != 0 && base == first && base != 31 {
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
        })
    }
}

impl Block {
    pub(super) fn memory(&mut self, pc: u64, retired: usize, w: u32) -> bool {
        let Some(t) = Transfer::decode(w) else {
            return false;
        };
        self.guarded = true;
        self.get(t.base, true);
        if t.displacement != 0 && !t.post {
            self.body.extend([I::I64Const(t.displacement), I::I64Add]);
        }
        self.body.push(I::LocalSet(ADDR));
        let bytes = t.size * (1 + u32::from(t.second.is_some()));
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
        if !t.load {
            self.body
                .extend([I::LocalGet(HOST), I::LocalGet(3), I::I32Eq, I::I32Or]);
        }
        self.body.extend([
            I::If(BlockType::Empty),
            I::I64Const(pc as i64),
            I::LocalSet(NEXT),
            I::LocalGet(RETIRED),
            I::I32Const(retired as i32),
            I::I32Add,
            I::LocalSet(RETIRED),
            I::Br(2),
            I::End,
            I::LocalGet(HOST),
            I::LocalGet(ADDR),
            I::I32WrapI64,
            I::I32Const(4095),
            I::I32And,
            I::I32Add,
            I::LocalSet(HOST),
        ]);
        for (i, r) in std::iter::once(t.first).chain(t.second).enumerate() {
            let arg = wasm_encoder::MemArg {
                align: t.size.ilog2(),
                ..at((i as u64) * u64::from(t.size))
            };
            self.body.push(I::LocalGet(HOST));
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
        if t.update {
            self.body.push(I::LocalGet(ADDR));
            if t.post {
                self.body.extend([I::I64Const(t.displacement), I::I64Add]);
            }
            self.set(t.base, true, true);
        }
        true
    }
}
