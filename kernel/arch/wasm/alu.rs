use super::cpu::{Cpu, Fault, sx};

fn mask(bits: u32) -> u64 {
    u64::MAX >> (64 - bits)
}
fn rotate(x: u64, r: u32, bits: u32) -> u64 {
    let r = r % bits;
    ((x >> r) | (x << ((bits - r) % bits))) & mask(bits)
}
fn shift(x: u64, kind: u32, amount: u32, bits: u32) -> u64 {
    let x = x & mask(bits);
    match kind {
        0 => x << amount,
        1 => x >> amount,
        2 => ((sx(x, bits) as i64) >> amount) as u64,
        _ => rotate(x, amount, bits),
    }
}
pub(super) fn logical_mask(n: u32, r: u32, s: u32, bits: u32) -> Option<u64> {
    let pattern = (n << 6) | (!s & 63);
    if pattern < 2 {
        return None;
    }
    let width = 1 << (31 - pattern.leading_zeros());
    let ones = (s & (width - 1)) + 1;
    if width > bits || ones == width {
        return None;
    }
    let elem = rotate(mask(ones), r, width);
    let mut value = elem;
    let mut span = width;
    while span < bits {
        value |= value << span;
        span *= 2;
    }
    Some(value)
}

impl Cpu {
    fn arithmetic(&mut self, x: u64, y: u64, sub: bool, flags: bool, bits: u32) -> u64 {
        let m = mask(bits);
        let (a, b) = (x & m, y & m);
        let value = if sub {
            a.wrapping_sub(b)
        } else {
            a.wrapping_add(b)
        } & m;
        if flags {
            let sign = 1 << (bits - 1);
            let carry = if sub { a >= b } else { value < a };
            let overflow = (if sub {
                (a ^ b) & (a ^ value)
            } else {
                !(a ^ b) & (a ^ value)
            }) & sign
                != 0;
            self.f.pstate = (self.f.pstate & !0xf000_0000)
                | ((value >> (bits - 1)) << 31)
                | ((value == 0) as u64) << 30
                | (carry as u64) << 29
                | (overflow as u64) << 28;
        }
        value
    }

    pub fn alu(&mut self, encoding: super::decode::Integer, w: u32) -> Result<(), Fault> {
        let bits = if w >> 31 != 0 { 64 } else { 32 };
        let (d, n, m) = (w & 31, (w >> 5) & 31, (w >> 16) & 31);
        let op = (w >> 29) & 3;
        let a = self.reg(n);
        let b = self.reg(m);
        let shift6 = (w >> 10) & 63;
        let mut use_sp = false;
        use super::decode::Integer::*;
        let value = match encoding {
            MoveWide => {
                let pos = ((w >> 21) & 3) * 16;
                if pos >= bits {
                    return Err(Fault::Instruction(w));
                }
                let imm = (((w >> 5) & 0xffff) as u64) << pos;
                match op {
                    0 => !imm,
                    2 => imm,
                    3 => (self.reg(d) & !(0xffff << pos)) | imm,
                    _ => return Err(Fault::Instruction(w)),
                }
            }
            AddCarry => {
                let a = a & mask(bits);
                let b = (if op & 2 != 0 { !b } else { b }) & mask(bits);
                let carry = (self.f.pstate >> 29) & 1;
                let sum = u128::from(a) + u128::from(b) + u128::from(carry);
                let value = sum as u64 & mask(bits);
                if op & 1 != 0 {
                    let negative = value >> (bits - 1);
                    let overflow = (!(a ^ b) & (a ^ value)) >> (bits - 1);
                    self.f.pstate = (self.f.pstate & !0xf0000000)
                        | (negative << 31)
                        | (((value == 0) as u64) << 30)
                        | (((sum >> bits) as u64) << 29)
                        | (overflow << 28);
                }
                value
            }
            AddImmediate => {
                let imm = (((w >> 10) & 4095) as u64) << (12 * ((w >> 22) & 1));
                use_sp = op & 1 == 0;
                self.arithmetic(self.base(n), imm, op & 2 != 0, op & 1 != 0, bits)
            }
            AddExtended => {
                let extend = (w >> 13) & 7;
                let amount = (w >> 10) & 7;
                if amount > 4 {
                    return Err(Fault::Instruction(w));
                }
                let width = 8 << (extend & 3);
                let operand = if extend & 4 != 0 {
                    sx(b, width)
                } else {
                    b & mask(width)
                };
                use_sp = op & 1 == 0;
                self.arithmetic(
                    self.base(n),
                    operand << amount,
                    op & 2 != 0,
                    op & 1 != 0,
                    bits,
                )
            }
            AddShifted => {
                let kind = (w >> 22) & 3;
                if kind == 3 || shift6 >= bits {
                    return Err(Fault::Instruction(w));
                }
                self.arithmetic(
                    a,
                    shift(b, kind, shift6, bits),
                    op & 2 != 0,
                    op & 1 != 0,
                    bits,
                )
            }
            Logical => {
                let imm = w & 0x1f800000 == 0x12000000;
                let operand = if imm {
                    logical_mask((w >> 22) & 1, (w >> 16) & 63, shift6, bits)
                        .ok_or(Fault::Instruction(w))?
                } else {
                    if shift6 >= bits {
                        return Err(Fault::Instruction(w));
                    }
                    let v = shift(b, (w >> 22) & 3, shift6, bits);
                    if w & (1 << 21) != 0 { !v } else { v }
                };
                use_sp = imm && op != 3;
                let v = match op {
                    0 | 3 => a & operand,
                    1 => a | operand,
                    _ => a ^ operand,
                } & mask(bits);
                if op == 3 {
                    self.f.pstate = (self.f.pstate & !0xf000_0000)
                        | ((v >> (bits - 1)) << 31)
                        | ((v == 0) as u64) << 30;
                }
                v
            }
            Extract => {
                if shift6 >= bits || (w >> 22) & 1 != w >> 31 {
                    return Err(Fault::Instruction(w));
                }
                if shift6 == 0 {
                    b
                } else {
                    ((b & mask(bits)) >> shift6) | a << (bits - shift6)
                }
            }
            Bitfield => {
                let r = (w >> 16) & 63;
                let s = shift6;
                if r >= bits || s >= bits || (w >> 22) & 1 != w >> 31 || op == 3 {
                    return Err(Fault::Instruction(w));
                }
                let (field, width, lsb) = if s >= r {
                    (a >> r, s - r + 1, 0)
                } else {
                    (a, s + 1, bits - r)
                };
                let v = (field & mask(width)) << lsb;
                match op {
                    0 => sx(v, width + lsb),
                    1 => (self.reg(d) & !(mask(width) << lsb)) | v,
                    _ => v,
                }
            }
            Select => {
                if self.condition((w >> 12) & 15) {
                    a
                } else {
                    let v = if w & (1 << 30) != 0 { !b } else { b };
                    v.wrapping_add(((w >> 10) & 1) as u64)
                }
            }
            Compare => {
                if self.condition((w >> 12) & 15) {
                    self.arithmetic(
                        a,
                        if w & 0x800 != 0 { m as u64 } else { b },
                        w & (1 << 30) != 0,
                        true,
                        bits,
                    );
                } else {
                    self.f.pstate = (self.f.pstate & !0xf000_0000) | (((w & 15) as u64) << 28);
                }
                return Ok(());
            }
            ReverseBits => (a & mask(bits)).reverse_bits() >> (64 - bits),
            ReverseBytes => match (w >> 10) & 3 {
                1 => (a & 0x00ff00ff00ff00ff) << 8 | (a >> 8) & 0x00ff00ff00ff00ff,
                2 => {
                    (a as u32).swap_bytes() as u64 | (((a >> 32) as u32).swap_bytes() as u64) << 32
                }
                _ => a.swap_bytes(),
            },
            LeadingSigns => {
                let sign = sx(a, bits) as i64 >> 63;
                (((a ^ sign as u64) & mask(bits)).leading_zeros() - (64 - bits) - 1) as u64
            }
            LeadingZeros => ((a & mask(bits)).leading_zeros() - (64 - bits)) as u64,
            VariableShift => shift(a, (w >> 10) & 3, (b as u32) & (bits - 1), bits),
            Divide => {
                if w & 0x400 != 0 {
                    let divisor = sx(b, bits) as i64;
                    if divisor == 0 {
                        0
                    } else {
                        (sx(a, bits) as i64).wrapping_div(divisor) as u64
                    }
                } else {
                    (a & mask(bits)).checked_div(b & mask(bits)).unwrap_or(0)
                }
            }
            MultiplyHigh => {
                if w & (1 << 23) != 0 {
                    umulh(a, b)
                } else {
                    ((a as i64 as i128 * b as i64 as i128) >> 64) as u64
                }
            }
            MultiplyLong => {
                let signed = w & (1 << 23) == 0;
                let extend = |x: u64| if signed { sx(x, 32) } else { x as u32 as u64 };
                let product = extend(a).wrapping_mul(extend(b));
                let acc = self.reg((w >> 10) & 31);
                if w & 0x8000 != 0 {
                    acc.wrapping_sub(product)
                } else {
                    acc.wrapping_add(product)
                }
            }
            MultiplyAdd => {
                let product = a.wrapping_mul(b);
                let acc = self.reg((w >> 10) & 31);
                if w & 0x8000 != 0 {
                    acc.wrapping_sub(product)
                } else {
                    acc.wrapping_add(product)
                }
            }
        };
        if use_sp {
            self.set_base(d, value & mask(bits));
        } else {
            self.set(d, value & mask(bits));
        }
        Ok(())
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn umulh(a: u64, b: u64) -> u64 {
    ((u128::from(a) * u128::from(b)) >> 64) as u64
}
