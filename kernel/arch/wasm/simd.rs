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
        if w & 0xfffffc00 == 0x9eae0000 {
            self.set(d as u32, (a >> 64) as u64);
            return Ok(());
        } else if w & 0xfffffc00 == 0x9eaf0000 {
            out = (self.context.v[d] & u64::MAX as u128) | (u128::from(self.reg(n)) << 64);
        } else if w & 0xfffffc00 == 0x1e260000 {
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
            || matches!(w & 0xbfe0fc00, 0x0e003c00 | 0x0e002c00)
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
            if w & 0xbfe0fc00 == 0x0e002c00 {
                let wide = w & (1 << 30) != 0;
                if width >= (if wide { 64 } else { 32 }) {
                    return Err(Fault::Instruction(w));
                }
                let value = super::cpu::sx((a >> shift) as u64, width);
                self.set(d as u32, if wide { value } else { value as u32 as u64 });
                return Ok(());
            }
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
        } else if w & 0xbfe0fc00 == 0x0e000400 || w & 0xffe0fc00 == 0x5e000400 {
            let imm = (w >> 16) & 31;
            let size = imm.trailing_zeros();
            if size > 3 {
                return Err(Fault::Instruction(w));
            }
            let bits = 8 << size;
            let idx = imm >> (size + 1);
            let value = (a >> (idx * bits)) & ((1 << bits) - 1);
            if w & (1 << 28) != 0 {
                out = value;
            } else {
                out = splat(value as u64, bits);
                if w & (1 << 30) == 0 {
                    if bits == 64 {
                        return Err(Fault::Instruction(w));
                    }
                    out &= u64::MAX as u128;
                }
            }
        } else if w & 0xbff0fc00 == 0x0e000c00 {
            let size = (w >> 16) & 15;
            if !size.is_power_of_two() {
                return Err(Fault::Instruction(w));
            }
            out = splat(self.reg(n), 8 * size);
            if w & (1 << 30) == 0 {
                if size == 8 {
                    return Err(Fault::Instruction(w));
                }
                out &= u64::MAX as u128;
            }
        } else if w & 0x8f800400 == 0x0f000400
            && w & 0x00780000 != 0
            && matches!((w >> 11) & 31, 0 | 2 | 4 | 6 | 8 | 10)
        {
            let imm = (w >> 16) & 127;
            let bits = 1 << imm.ilog2();
            let u = w & (1 << 29) != 0;
            let scalar = w & (1 << 28) != 0;
            let count = if scalar {
                1
            } else {
                (if w & (1 << 30) != 0 { 128 } else { 64 }) / bits
            };
            if scalar && bits != 64 || !scalar && count == 1 {
                return Err(Fault::Instruction(w));
            }
            let elem_mask = (1u128 << bits) - 1;
            let op = (w >> 11) & 31;
            for i in 0..count {
                let old = (self.context.v[d] >> (i * bits)) & elem_mask;
                let x = (a >> (i * bits)) & elem_mask;
                let v = if op == 10 {
                    let shift = imm - bits;
                    (x << shift) | (if u { old & ((1 << shift) - 1) } else { 0 })
                } else {
                    let shift = 2 * bits - imm;
                    if op == 8 {
                        if !u {
                            return Err(Fault::Instruction(w));
                        }
                        (old & !(elem_mask >> shift)) | (x >> shift)
                    } else {
                        let value = if !u && x & (1 << (bits - 1)) != 0 {
                            (x | !elem_mask) as i128
                        } else {
                            x as i128
                        };
                        let rounded = value + (if op & 4 != 0 { 1 << (shift - 1) } else { 0 });
                        ((rounded >> shift) as u128).wrapping_add(if op & 2 != 0 { old } else { 0 })
                    }
                };
                out |= (v & elem_mask) << (i * bits);
            }
        } else if w & 0xbf3ffc00 == 0x2e213800 {
            if width == 64 {
                return Err(Fault::Instruction(w));
            }
            let start = if w & (1 << 30) != 0 { lanes / 2 } else { 0 };
            for i in 0..lanes / 2 {
                out |= lane(a, start + i) << ((2 * i + 1) * width);
            }
        } else if w & 0xbf80f400 == 0x0f008400 && w & 0x00780000 != 0 {
            let imm = (w >> 16) & 127;
            if !(8..64).contains(&imm) {
                return Err(Fault::Instruction(w));
            }
            let bits = 1 << imm.ilog2();
            let shift = 2 * bits - imm;
            let round = if w & (1 << 11) != 0 {
                1 << (shift - 1)
            } else {
                0
            };
            let elem_mask = (1u128 << bits) - 1;
            for i in 0..64 / bits {
                let input = (a >> (i * 2 * bits)) & ((1u128 << (2 * bits)) - 1);
                out |= (((input + round) >> shift) & elem_mask) << (i * bits);
            }
            if w & (1 << 30) != 0 {
                out = (out << 64) | (self.context.v[d] & u64::MAX as u128);
            }
        } else if w & 0x9f80fc00 == 0x0f00a400 && w & 0x00780000 != 0 {
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
            w & 0x8f20fc00,
            0x0e208400 | 0x0e208c00 | 0x0e203400 | 0x0e203c00
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
        } else if matches!(w & 0x8f20fc00, 0x0e204400 | 0x0e205400) {
            let scalar = w & (1 << 28) != 0;
            let count = if scalar {
                1
            } else if w & (1 << 30) != 0 {
                lanes
            } else {
                lanes / 2
            };
            if scalar != (count == 1 && width == 64) {
                return Err(Fault::Instruction(w));
            }
            let signed = w & (1 << 29) == 0;
            for i in 0..count {
                let x = lane(a, i);
                let shift = lane(b, i) as i8 as i16;
                let y = if shift >= 0 {
                    if shift as u32 >= width { 0 } else { x << shift }
                } else {
                    let amount = (-shift) as u32;
                    let v = if signed {
                        ((x << (128 - width)) as i128) >> (128 - width)
                    } else {
                        x as i128
                    };
                    if amount > width {
                        if signed && v < 0 && w & 0x1000 == 0 {
                            mask
                        } else {
                            0
                        }
                    } else {
                        let round = if w & 0x1000 == 0 {
                            0
                        } else {
                            1 << (amount - 1)
                        };
                        ((v + round) >> amount) as u128
                    }
                };
                out |= (y & mask) << (i * width);
            }
        } else if w & 0x9f20fc00 == 0x0e209400 || w & 0xbf20fc00 == 0x0e209c00 {
            if width == 64 {
                return Err(Fault::Instruction(w));
            }
            let count = if w & (1 << 30) != 0 { lanes } else { lanes / 2 };
            for i in 0..count {
                let product = lane(a, i) * lane(b, i);
                let acc = lane(self.context.v[d], i);
                let value = if w & 0x800 != 0 {
                    product
                } else if w & (1 << 29) != 0 {
                    acc.wrapping_sub(product)
                } else {
                    acc + product
                };
                out |= (value & mask) << (i * width);
            }
        } else if w & 0x9f3efc00 == 0x0e30a800 {
            let count = if w & (1 << 30) != 0 { lanes } else { lanes / 2 };
            if count < 4 || width > 32 {
                return Err(Fault::Instruction(w));
            }
            let sign = if w & (1 << 29) == 0 {
                1 << (width - 1)
            } else {
                0
            };
            out = lane(a, 0);
            for i in 1..count {
                let x = lane(a, i);
                if ((x ^ sign) > (out ^ sign)) != (w & (1 << 16) != 0) {
                    out = x;
                }
            }
        } else if w & 0x9f20cc00 == 0x0e200000 {
            if width >= 64 {
                return Err(Fault::Instruction(w));
            }
            let start = if w & (1 << 30) == 0 { 0 } else { lanes / 2 };
            let wide_mask = (1 << (2 * width)) - 1;
            let extend = |v: u128| {
                if w & (1 << 29) == 0 && (v >> (width - 1)) != 0 {
                    v | !mask
                } else {
                    v
                }
            };
            for i in 0..lanes / 2 {
                let x = if w & (1 << 12) != 0 {
                    (a >> (2 * i * width)) & wide_mask
                } else {
                    extend(lane(a, start + i))
                };
                let y = extend(lane(b, start + i));
                let value = if w & (1 << 13) == 0 {
                    x.wrapping_add(y)
                } else {
                    x.wrapping_sub(y)
                };
                out |= (value & wide_mask) << (2 * i * width);
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
        } else if w & 0x9fbffc00 == 0x0ea0f800 {
            let bits = if w & (1 << 22) == 0 { 32 } else { 64 };
            if bits == 64 && w & (1 << 30) == 0 {
                return Err(Fault::Instruction(w));
            }
            let signs = splat(1 << (bits - 1), bits);
            out = if w & (1 << 29) == 0 {
                a & !signs
            } else {
                a ^ signs
            };
            if w & (1 << 30) == 0 {
                out &= u64::MAX as u128;
            }
        } else if w & 0xfffffc00 == 0x5ef1b800 {
            out = (a as u64).wrapping_add((a >> 64) as u64) as u128;
        } else if matches!(w & 0xbf3ffc00, 0x0e200800 | 0x2e200800 | 0x0e201800) {
            let block = match w & 0x20001000 {
                0 => 64,
                0x1000 => 16,
                _ => 32,
            };
            if width >= block {
                return Err(Fault::Instruction(w));
            }
            let count = if w & (1 << 30) != 0 { lanes } else { lanes / 2 };
            for i in 0..count {
                out |= lane(a, i ^ (block / width - 1)) << (i * width);
            }
        } else if w & 0xbfbffc00 == 0x2e205800 {
            out = if w & (1 << 22) == 0 {
                !a
            } else {
                u128::from_le_bytes(a.to_le_bytes().map(u8::reverse_bits))
            };
            if w & (1 << 30) == 0 {
                out &= u64::MAX as u128;
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
        } else if matches!(w & 0x9f20fc00, 0x0e20a400 | 0x0e20ac00) || w & 0xbf20fc00 == 0x0e20bc00
        {
            let count = if w & (1 << 30) != 0 { lanes } else { lanes / 2 };
            let add = w & 0xf800 == 0xb800;
            if count < 2 || (width == 64 && !add) {
                return Err(Fault::Instruction(w));
            }
            let sign = if w & (1 << 29) == 0 {
                1 << (width - 1)
            } else {
                0
            };
            for i in 0..count {
                let src = if i < count / 2 { a } else { b };
                let j = (i % (count / 2)) * 2;
                let (x, y) = (lane(src, j), lane(src, j + 1));
                let value = if add {
                    (x + y) & mask
                } else if ((x ^ sign) > (y ^ sign)) != (w & 0x800 != 0) {
                    x
                } else {
                    y
                };
                out |= value << (i * width);
            }
        } else {
            return Err(Fault::Instruction(w));
        }
        self.context.v[d] = out;
        Ok(())
    }
}
