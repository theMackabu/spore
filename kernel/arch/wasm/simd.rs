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
        if w & 0x8fbffc00 == 0x0e21d800 {
            return self.convert_lanes(w);
        }
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
        } else if w & 0x9ff80c00 == 0x0f000400 {
            let imm = u64::from(((w >> 16) & 7) << 5 | (w >> 5) & 31);
            let mode = (w >> 12) & 15;
            let invert = w & (1 << 29) != 0;
            out = match mode {
                0..=7 => splat(imm << (8 * (mode / 2)), 32),
                8..=11 => splat(imm << (8 * ((mode - 8) / 2)), 16),
                12 => splat((imm << 8) | 255, 32),
                13 => splat((imm << 16) | 65535, 32),
                14 if !invert => splat(imm, 8),
                14 => {
                    let mut pattern = 0;
                    for byte in 0..8 {
                        if imm & (1 << byte) != 0 {
                            pattern |= 255 << (8 * byte);
                        }
                    }
                    splat(pattern, 64)
                }
                15 if !invert || w & (1 << 30) != 0 => splat(
                    super::float::expand_immediate(imm as u32, invert),
                    if invert { 64 } else { 32 },
                ),
                _ => return Err(Fault::Instruction(w)),
            };
            if invert && mode < 14 {
                out = !out;
            }
            if mode < 12 && mode & 1 != 0 {
                out = if invert {
                    self.context.v[d] & out
                } else {
                    self.context.v[d] | out
                };
            }
            if w & (1 << 30) == 0 {
                out &= u64::MAX as u128;
            }
        } else if matches!(
            w & 0x9f20fc00,
            0x0e208400 | 0x0e208c00 | 0x0e203400 | 0x0e203c00
        ) {
            let count = if w & (1 << 30) != 0 { lanes } else { lanes / 2 };
            if width == 64 && count == 1 {
                return Err(Fault::Instruction(w));
            }
            let u = w & (1 << 29) != 0;
            for i in 0..count {
                let x = lane(a, i);
                let y = lane(b, i);
                let v = match w & 0xfc00 {
                    0x8400 if u => x.wrapping_sub(y) & mask,
                    0x8400 => (x + y) & mask,
                    0x8c00 => {
                        if (u && x == y) || (!u && x & y != 0) {
                            mask
                        } else {
                            0
                        }
                    }
                    _ => {
                        let sign = if u { 0 } else { 1u128 << (width - 1) };
                        if (x ^ sign) > (y ^ sign) || (x == y && w & 0x800 != 0) {
                            mask
                        } else {
                            0
                        }
                    }
                };
                out |= v << (i * width);
            }
        } else if w & 0xbf3ffc00 == 0x0e31b800 {
            let count = if w & (1 << 30) != 0 { lanes } else { lanes / 2 };
            if width >= 64 || count < 4 {
                return Err(Fault::Instruction(w));
            }
            out = (0..count).map(|i| lane(a, i)).sum::<u128>() & mask;
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
            w & 0xaf3ffc00,
            0x0e208800 | 0x2e208800 | 0x0e209800 | 0x2e209800 | 0x0e20a800
        ) {
            let scalar = w & (1 << 28) != 0;
            let count = if scalar {
                1
            } else if w & (1 << 30) != 0 {
                lanes
            } else {
                lanes / 2
            };
            if scalar != (width == 64 && count == 1) {
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
        } else if w & 0xbf3ffc00 == 0x0e212800 && width < 64 {
            for i in 0..lanes / 2 {
                out |= ((a >> (i * width * 2)) & mask) << (i * width);
            }
            if w & (1 << 30) != 0 {
                out = (out << 64) | (self.context.v[d] & u64::MAX as u128);
            }
        } else if matches!(w & 0xbf20bc00, 0x0e003800 | 0x0e001800 | 0x0e002800) {
            let count = if w & (1 << 30) != 0 { lanes } else { lanes / 2 };
            if count < 2 {
                return Err(Fault::Instruction(w));
            }
            let part = (w >> 14) & 1;
            for i in 0..count / 2 {
                match (w >> 12) & 3 {
                    1 => {
                        out |= lane(a, 2 * i + part) << (i * width);
                        out |= lane(b, 2 * i + part) << ((count / 2 + i) * width);
                    }
                    op => {
                        let j = if op == 2 {
                            2 * i + part
                        } else {
                            part * count / 2 + i
                        };
                        out |= (lane(a, j) | lane(b, j) << width) << (2 * i * width);
                    }
                }
            }
        } else if w & 0xbffffc00 == 0x0e205800 {
            out = u128::from_le_bytes(a.to_le_bytes().map(|b| b.count_ones() as u8));
            if w & (1 << 30) == 0 {
                out &= u64::MAX as u128;
            }
        } else if w & 0xbfe08400 == 0x2e000000 {
            let bytes = if w & (1 << 30) != 0 { 16 } else { 8 };
            let start = (w >> 11) & 15;
            if start >= bytes {
                return Err(Fault::Instruction(w));
            }
            for i in 0..bytes {
                let j = start + i;
                let src = if j < bytes { a } else { b };
                out |= ((src >> (8 * (j % bytes))) & 255) << (8 * i);
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
