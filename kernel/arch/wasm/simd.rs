use super::cpu::{Cpu, Fault};

fn splat(value: u64, width: u32) -> u128 {
    let mask = (1u128 << width) - 1;
    (u128::from(value) & mask) * (u128::MAX / mask)
}

impl Cpu {
    pub fn simd(&mut self, w: u32) -> Result<(), Fault> {
        let d = (w & 31) as usize;
        let n = (w >> 5) & 31;
        let m = (w >> 16) & 31;
        let a = self.context.v[n as usize];
        let b = self.context.v[m as usize];
        let width = 8 << ((w >> 22) & 3);
        let lanes = 128 / width;
        let mask = (1u128 << width) - 1;
        let lane = |v: u128, i: u32| (v >> (width * i)) & mask;
        let mut out = 0;
        if w & 0xfffffc00 == 0x1e260000 {
            self.set(d as u32, a as u32 as u64);
            return Ok(());
        } else if w & 0xfffffc00 == 0x1e270000 {
            out = self.reg(n) as u32 as u128;
        } else if w & 0xfffffc00 == 0x9e670000 {
            out = u128::from(self.reg(n));
        } else if w & 0xfffffc00 == 0x9e660000 {
            self.set(d as u32, a as u64);
            return Ok(());
        } else if w & 0xffe0fc00 == 0x4e001c00
            || w & 0xbfe0fc00 == 0x0e003c00
            || w & 0xffe08400 == 0x6e000400
        {
            let imm = (w >> 16) & 31;
            let size = imm.trailing_zeros();
            if size > 3 {
                return Err(Fault::Instruction(w));
            }
            let width = 8 << size;
            let shift = (imm >> (size + 1)) * width;
            let mask = (1u128 << width) - 1;
            if w & 0xbfe0fc00 == 0x0e003c00 {
                if (width == 64) != (w & (1 << 30) != 0) {
                    return Err(Fault::Instruction(w));
                }
                self.set(d as u32, ((a >> shift) & mask) as u64);
                return Ok(());
            }
            let value = if w & (1 << 29) == 0 {
                u128::from(self.reg(n))
            } else {
                let lane = (w >> 11) & 15;
                if lane & ((1 << size) - 1) != 0 {
                    return Err(Fault::Instruction(w));
                }
                a >> ((lane >> size) * width)
            };
            out = (self.context.v[d] & !(mask << shift)) | ((value & mask) << shift);
        } else if w & 0xfff0fc00 == 0x4e000c00 {
            let size = (w >> 16) & 15;
            if !size.is_power_of_two() {
                return Err(Fault::Instruction(w));
            }
            out = splat(self.reg(n), 8 * size);
        } else if w & 0x9f80fc00 == 0x0f00a400 {
            let imm = (w >> 16) & 127;
            if !(8..64).contains(&imm) {
                return Err(Fault::Instruction(w));
            }
            let width = 1 << imm.ilog2();
            let start = if w & (1 << 30) != 0 { 64 } else { 0 };
            let mask = (1u128 << width) - 1;
            for lane in 0..64 / width {
                let src = (a >> (start + lane * width)) & mask;
                let value = if w & (1 << 29) == 0 && src >> (width - 1) != 0 {
                    src | (mask << width)
                } else {
                    src
                };
                out |= ((value << (imm - width)) & ((1 << (2 * width)) - 1)) << (lane * 2 * width);
            }
        } else if w & 0xbff8fc00 == 0x2f00e400 {
            let imm = ((w >> 16) & 7) << 5 | (w >> 5) & 31;
            let mut elem = 0u64;
            for byte in 0..8 {
                if imm & (1 << byte) != 0 {
                    elem |= 0xff << (byte * 8);
                }
            }
            out = if w & (1 << 30) != 0 {
                splat(elem, 64)
            } else {
                u128::from(elem)
            };
        } else if w & 0xbff8fc00 == 0x0f00e400 {
            let imm = ((w >> 16) & 7) << 5 | (w >> 5) & 31;
            out = splat(imm as u64, 8);
            if w & (1 << 30) == 0 {
                out &= u64::MAX as u128;
            }
        } else if w & 0x9ff89c00 == 0x0f000400 {
            let imm = ((w >> 16) & 7) << 5 | (w >> 5) & 31;
            out = splat((imm as u64) << (((w >> 13) & 3) * 8), 32);
            if w & (1 << 29) != 0 {
                out = !out;
            }
            if w & (1 << 30) == 0 {
                out &= u64::MAX as u128;
            }
        } else if w & 0xff20fc00 == 0x4e208400 || w & 0xff20fc00 == 0x6e208c00 {
            for i in 0..lanes {
                let x = lane(a, i);
                let y = lane(b, i);
                let value = if w >> 29 & 1 != 0 {
                    if x == y { mask } else { 0 }
                } else {
                    (x + y) & mask
                };
                out |= value << (i * width);
            }
        } else if w & 0x9f20fc00 == 0x0e201c00 {
            let old = self.context.v[d];
            out = match ((w >> 27) & 4) | ((w >> 22) & 3) {
                0 => a & b,
                1 => a & !b,
                2 => a | b,
                3 => a | !b,
                4 => a ^ b,
                5 => (old & a) | (!old & b),
                6 => (old & !b) | (a & b),
                _ => (old & b) | (a & !b),
            };
            if w & (1 << 30) == 0 {
                out &= u64::MAX as u128;
            }
        } else if matches!(
            w & 0xbf3ffc00,
            0x0e208800 | 0x2e208800 | 0x0e209800 | 0x2e209800 | 0x0e20a800
        ) {
            let count = if w & (1 << 30) != 0 { lanes } else { lanes / 2 };
            if width == 64 && count == 1 {
                return Err(Fault::Instruction(w));
            }
            for i in 0..count {
                let x = lane(a, i);
                let negative = (x >> (width - 1)) != 0;
                let yes = match w & 0x20003000 {
                    0 => !negative && x != 0,
                    0x20000000 => !negative,
                    0x1000 => x == 0,
                    0x20001000 => negative || x == 0,
                    _ => negative,
                };
                if yes {
                    out |= mask << (i * width);
                }
            }
        } else if w & 0xff20bc00 == 0x4e003800 {
            let start = if w & 0x4000 != 0 { lanes / 2 } else { 0 };
            for i in 0..lanes / 2 {
                out |= (lane(a, start + i) | lane(b, start + i) << width) << (2 * i * width);
            }
        } else if w & 0xffe0fc00 == 0x4e000000 {
            let table = a.to_le_bytes();
            out = u128::from_le_bytes(
                b.to_le_bytes()
                    .map(|index| table.get(index as usize).copied().unwrap_or(0)),
            );
        } else if w & 0xff20fc00 == 0x6e20a400 && width < 64 {
            for i in 0..lanes {
                let src = if i < lanes / 2 { a } else { b };
                let j = (i % (lanes / 2)) * 2;
                out |= lane(src, j).max(lane(src, j + 1)) << (i * width);
            }
        } else {
            return Err(Fault::Instruction(w));
        }
        self.context.v[d] = out;
        Ok(())
    }
}
