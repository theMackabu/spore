use super::cpu::{Cpu, Fault};

#[derive(Clone, Copy)]
pub enum Family {
    Binary,
    Unary,
    Wide,
    NarrowHigh,
    NarrowSat,
    PairLong,
    Table,
    SatShift,
    NarrowShift,
    ImmediateSat,
    MultiplyLane,
}

pub fn classify(w: u32) -> Option<Family> {
    use Family::*;
    Some(
        if matches!(
            w & 0x9f20fc00,
            0x0e200400
                | 0x0e201400
                | 0x0e202400
                | 0x0e207400
                | 0x0e207c00
                | 0x0e206400
                | 0x0e206c00
        ) || matches!(w & 0x8f20fc00, 0x0e200c00 | 0x0e202c00 | 0x0e20b400)
        {
            Binary
        } else if matches!(w & 0x8f3ffc00, 0x0e207800 | 0x0e20b800 | 0x0e203800)
            || w & 0x9f3ffc00 == 0x0e204800
        {
            Unary
        } else if matches!(
            w & 0x9f20fc00,
            0x0e205000 | 0x0e207000 | 0x0e208000 | 0x0e20a000 | 0x0e20c000
        ) || matches!(w & 0xaf20fc00, 0x0e209000 | 0x0e20b000 | 0x0e20d000)
        {
            Wide
        } else if matches!(w & 0x9f20fc00, 0x0e204000 | 0x0e206000) {
            NarrowHigh
        } else if w & 0x8f3ffc00 == 0x0e214800 || w & 0xaf3ffc00 == 0x2e212800 {
            NarrowSat
        } else if matches!(w & 0x9f3ffc00, 0x0e202800 | 0x0e206800 | 0x0e303800) {
            PairLong
        } else if w & 0xbfe08c00 == 0x0e000000 {
            Table
        } else if matches!(w & 0x8f20fc00, 0x0e204c00 | 0x0e205c00) {
            SatShift
        } else if w & 0x00780000 != 0
            && (w & 0x8f80f400 == 0x0f009400 || w & 0xaf80f400 == 0x2f008400)
        {
            NarrowShift
        } else if w & 0x00780000 != 0
            && (w & 0x8f80fc00 == 0x0f007400 || w & 0xaf80fc00 == 0x2f006400)
        {
            ImmediateSat
        } else if w & 0x0f000000 == 0x0f000000
            && matches!(
                (w >> 12) & 15,
                0 | 2 | 3 | 4 | 6 | 7 | 8 | 10 | 11 | 12 | 13
            )
            && w & 0x400 == 0
            && matches!((w >> 22) & 3, 1 | 2)
        {
            MultiplyLane
        } else {
            return None;
        },
    )
}

fn mask(bits: u32) -> u128 {
    (1u128 << bits) - 1
}
fn lane(v: u128, i: u32, bits: u32, signed: bool) -> i128 {
    let raw = (v >> (i * bits)) & mask(bits);
    if signed {
        ((raw << (128 - bits)) as i128) >> (128 - bits)
    } else {
        raw as i128
    }
}
fn put(out: &mut u128, i: u32, bits: u32, value: i128) {
    *out |= ((value as u128) & mask(bits)) << (i * bits);
}
fn saturate(x: i128, bits: u32, signed: bool, qc: &mut bool) -> i128 {
    let hi = if signed {
        (1i128 << (bits - 1)) - 1
    } else {
        mask(bits) as i128
    };
    let lo = if signed { -hi - 1 } else { 0 };
    let result = x.clamp(lo, hi);
    *qc |= result != x;
    result
}
fn shift_sat(x: i128, amount: i32, bits: u32, signed: bool, round: bool, qc: &mut bool) -> i128 {
    if amount < 0 {
        let shift = amount.unsigned_abs();
        if shift > bits {
            return if x < 0 && !round { -1 } else { 0 };
        }
        let rounding = if round { 1i128 << (shift - 1) } else { 0 };
        return (x + rounding) >> shift;
    }
    if amount as u32 >= bits && x != 0 {
        return saturate(if x < 0 { i128::MIN } else { i128::MAX }, bits, signed, qc);
    }
    if x == 0 {
        0
    } else {
        saturate(x << amount, bits, signed, qc)
    }
}
struct Operands {
    old: u128,
    a: u128,
    b: u128,
    bits: u32,
    upper: bool,
    scalar: bool,
    signed: bool,
}
impl Operands {
    fn x(&self, i: u32) -> i128 {
        lane(self.a, i, self.bits, self.signed)
    }
    fn y(&self, i: u32) -> i128 {
        lane(self.b, i, self.bits, self.signed)
    }
    fn wide_index(&self, i: u32) -> u32 {
        if self.upper && !self.scalar {
            64 / self.bits + i
        } else {
            i
        }
    }
    fn narrowed(&self, out: u128) -> u128 {
        if self.upper && !self.scalar {
            (self.old & u128::from(u64::MAX)) | (out << 64)
        } else {
            out
        }
    }
}

impl Cpu {
    pub fn neon_integer(&mut self, family: Family, w: u32) -> Result<(), Fault> {
        let rd = (w & 31) as usize;
        let rn = ((w >> 5) & 31) as usize;
        let rm = ((w >> 16) & 31) as usize;
        let scalar = w & (1 << 28) != 0;
        let upper = w & (1 << 30) != 0;
        let signed = w & (1 << 29) == 0;
        let bits = 8 << ((w >> 22) & 3);
        let count = if scalar {
            1
        } else if upper {
            128 / bits
        } else {
            64 / bits
        };
        let v = Operands {
            old: self.context.v[rd],
            a: self.context.v[rn],
            b: self.context.v[rm],
            bits,
            upper,
            scalar,
            signed,
        };
        let mut qc = false;
        let mut out = 0;
        use Family::*;
        match family {
            Binary => {
                let op = (w >> 10) & 63;
                if bits == 64 && (!scalar && !upper || !matches!(op, 3 | 11))
                    || op == 45 && !matches!(bits, 16 | 32)
                {
                    return Err(Fault::Instruction(w));
                }
                for i in 0..count {
                    let (x, y) = (v.x(i), v.y(i));
                    let val = match op {
                        1 => (x + y) >> 1,
                        5 => (x + y + 1) >> 1,
                        9 => (x - y) >> 1,
                        3 => saturate(x + y, bits, signed, &mut qc),
                        11 => saturate(x - y, bits, signed, &mut qc),
                        25 => x.max(y),
                        27 => x.min(y),
                        29 => (x - y).abs(),
                        31 => lane(v.old, i, bits, false) + (x - y).abs(),
                        45 => {
                            let p = 2 * lane(v.a, i, bits, true) * lane(v.b, i, bits, true);
                            saturate(
                                (p + if signed { 0 } else { 1 << (bits - 1) }) >> bits,
                                bits,
                                true,
                                &mut qc,
                            )
                        }
                        _ => return Err(Fault::Instruction(w)),
                    };
                    put(&mut out, i, bits, val);
                }
            }
            Unary => {
                let op = (w >> 12) & 15;
                if bits == 64 && !scalar && !upper || op == 4 && bits == 64 {
                    return Err(Fault::Instruction(w));
                }
                for i in 0..count {
                    let x = lane(v.a, i, bits, true);
                    let val = match op {
                        3 => saturate(
                            lane(v.old, i, bits, signed) + lane(v.a, i, bits, !signed),
                            bits,
                            signed,
                            &mut qc,
                        ),
                        4 => {
                            let raw = lane(v.a, i, bits, false) as u128;
                            let scan = if signed && x < 0 {
                                raw ^ mask(bits)
                            } else {
                                raw
                            };
                            i128::from((scan << (128 - bits)).leading_zeros().min(bits))
                                - i128::from(signed)
                        }
                        7 => saturate(if signed { x.abs() } else { -x }, bits, true, &mut qc),
                        11 => {
                            if signed {
                                x.abs()
                            } else {
                                -x
                            }
                        }
                        _ => return Err(Fault::Instruction(w)),
                    };
                    put(&mut out, i, bits, val);
                }
            }
            Wide | MultiplyLane => {
                let indexed = matches!(family, MultiplyLane);
                let op = (w >> 12) & 15;
                let high = indexed && op >= 12;
                let narrow = indexed && matches!(op, 0 | 4 | 8) || high;
                if bits == 64 || indexed && !matches!(bits, 16 | 32) {
                    return Err(Fault::Instruction(w));
                }
                let doubling = if indexed {
                    matches!(op, 3 | 7 | 11 | 12 | 13)
                } else {
                    op & 1 != 0 && op >= 9
                };
                if doubling && bits == 8 {
                    return Err(Fault::Instruction(w));
                }
                let subtract = if indexed {
                    matches!(op, 4 | 6 | 7)
                } else {
                    matches!(op, 10 | 11)
                };
                let accumulate = if indexed {
                    op < 8
                } else {
                    matches!(op, 5 | 8 | 9 | 10 | 11)
                };
                let lanes = if scalar {
                    1
                } else if narrow {
                    count
                } else {
                    64 / bits
                };
                let dest_bits = if narrow { bits } else { bits * 2 };
                let h = (w >> 11) & 1;
                let l = (w >> 21) & 1;
                let idx = if bits == 16 {
                    (h << 2) | (l << 1) | ((w >> 20) & 1)
                } else {
                    (h << 1) | l
                };
                let b = if indexed && bits == 16 {
                    self.context.v[rm & 15]
                } else {
                    v.b
                };
                let use_signed = signed || doubling;
                for i in 0..lanes {
                    let j = if narrow { i } else { v.wide_index(i) };
                    let x = lane(v.a, j, bits, use_signed);
                    let y = lane(b, if indexed { idx } else { j }, bits, use_signed);
                    let product = if !indexed && matches!(op, 5 | 7) {
                        (x - y).abs()
                    } else {
                        x * y
                    };
                    let mut val = if high {
                        saturate(
                            (product * 2 + if op == 13 { 1 << (bits - 1) } else { 0 }) >> bits,
                            bits,
                            true,
                            &mut qc,
                        )
                    } else if doubling {
                        saturate(product * 2, dest_bits, true, &mut qc)
                    } else {
                        product
                    };
                    if accumulate {
                        let acc = lane(v.old, i, dest_bits, doubling);
                        val = if subtract { acc - val } else { acc + val };
                        if doubling {
                            val = saturate(val, dest_bits, true, &mut qc);
                        }
                    }
                    put(&mut out, i, dest_bits, val);
                }
            }
            NarrowHigh => {
                if bits == 64 {
                    return Err(Fault::Instruction(w));
                }
                for i in 0..64 / bits {
                    let (x, y) = (lane(v.a, i, bits * 2, false), lane(v.b, i, bits * 2, false));
                    let val = if w & 0x2000 != 0 { x - y } else { x + y };
                    put(
                        &mut out,
                        i,
                        bits,
                        (val + if signed { 0 } else { 1 << (bits - 1) }) >> bits,
                    );
                }
                out = v.narrowed(out);
            }
            NarrowSat | NarrowShift => {
                let shift = matches!(family, NarrowShift);
                let imm = (w >> 16) & 127;
                let width = if shift { 1 << imm.ilog2() } else { bits };
                if width == 64 {
                    return Err(Fault::Instruction(w));
                }
                let signed_input = signed
                    || if shift {
                        w & 0x1000 == 0
                    } else {
                        w & 0x2000 != 0
                    };
                let amount = if shift { 2 * width - imm } else { 0 };
                let round = if shift && w & 0x800 != 0 {
                    1 << (amount - 1)
                } else {
                    0
                };
                let lanes = if scalar { 1 } else { 64 / width };
                for i in 0..lanes {
                    let x = lane(v.a, i, 2 * width, signed_input);
                    put(
                        &mut out,
                        i,
                        width,
                        saturate((x + round) >> amount, width, signed, &mut qc),
                    );
                }
                out = v.narrowed(out);
            }
            PairLong => {
                if bits == 64 {
                    return Err(Fault::Instruction(w));
                }
                if w & 0x100000 != 0 {
                    if bits == 32 && !upper {
                        return Err(Fault::Instruction(w));
                    }
                    put(&mut out, 0, bits * 2, (0..count).map(|i| v.x(i)).sum());
                } else {
                    for i in 0..count / 2 {
                        let acc = if w & 0x4000 != 0 {
                            lane(v.old, i, 2 * bits, false)
                        } else {
                            0
                        };
                        put(&mut out, i, bits * 2, acc + v.x(2 * i) + v.x(2 * i + 1));
                    }
                }
            }
            Table => {
                let tables = ((w >> 13) & 3) + 1;
                for i in 0..if upper { 16 } else { 8 } {
                    let which = lane(v.b, i, 8, false) as u32;
                    let val = if which < tables * 16 {
                        lane(
                            self.context.v[(rn + (which / 16) as usize) % 32],
                            which % 16,
                            8,
                            false,
                        )
                    } else if w & 0x1000 != 0 {
                        lane(v.old, i, 8, false)
                    } else {
                        0
                    };
                    put(&mut out, i, 8, val);
                }
            }
            SatShift | ImmediateSat => {
                let imm = (w >> 16) & 127;
                let immediate = matches!(family, ImmediateSat);
                let width = if immediate { 1 << imm.ilog2() } else { bits };
                let signed_input = signed || immediate && w & 0x1000 == 0;
                let lanes = if scalar {
                    1
                } else if upper {
                    128 / width
                } else {
                    64 / width
                };
                if lanes == 1 && !scalar && width == 64 {
                    return Err(Fault::Instruction(w));
                }
                for i in 0..lanes {
                    let amount = if immediate {
                        (imm - width) as i32
                    } else {
                        lane(v.b, i, bits, false) as i8 as i32
                    };
                    let x = lane(v.a, i, width, signed_input);
                    let val = shift_sat(
                        x,
                        amount,
                        width,
                        signed,
                        !immediate && w & 0x1000 != 0,
                        &mut qc,
                    );
                    put(&mut out, i, width, val);
                }
            }
        }
        if qc {
            self.context.fpsr |= 1 << 27;
        }
        self.context.v[rd] = out;
        Ok(())
    }
}
