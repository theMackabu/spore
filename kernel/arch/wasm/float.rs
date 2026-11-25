use super::cpu::{Cpu, Fault};
use rustc_apfloat::{
    Float, FloatConvert, Round, Status, StatusAnd,
    ieee::{Double, Single},
};

pub fn expand_immediate(imm: u32, wide: bool) -> u64 {
    let (fraction, exponent) = if wide { (52, 11) } else { (23, 8) };
    let b = (imm >> 6) & 1;
    let exp = ((1 - b) << (exponent - 1)) | (b * ((1 << (exponent - 1)) - 4)) | ((imm >> 4) & 3);
    (u64::from(imm >> 7) << (fraction + exponent))
        | (u64::from(exp) << fraction)
        | (u64::from(imm & 15) << (fraction - 4))
}

fn rounding(bits: u32) -> Round {
    match bits & 3 {
        0 => Round::NearestTiesToEven,
        1 => Round::TowardPositive,
        2 => Round::TowardNegative,
        _ => Round::TowardZero,
    }
}

fn from_integer<F: Float>(value: u64, bits: usize, signed: bool, round: Round) -> StatusAnd<F> {
    match (signed, bits) {
        (true, 32) => F::from_i128_r(value as i32 as i128, round),
        (true, _) => F::from_i128_r(value as i64 as i128, round),
        (false, 32) => F::from_u128_r(value as u32 as u128, round),
        (false, _) => F::from_u128_r(value as u128, round),
    }
}

fn minmax<F: Float>(a: F, b: F, minimum: bool, numeric: bool) -> StatusAnd<F> {
    if a.is_signaling() || b.is_signaling() {
        let nan = if a.is_signaling() { a } else { b };
        return Status::INVALID_OP.and(F::from_bits(nan.to_bits() | (1 << (F::PRECISION - 2))));
    }
    if numeric && a.is_nan() != b.is_nan() {
        return Status::OK.and(if a.is_nan() { b } else { a });
    }
    Status::OK.and(if minimum { a.minimum(b) } else { a.maximum(b) })
}

impl Cpu {
    fn fp_input<F: Float>(&mut self, reg: usize) -> F {
        self.flush_input(F::from_bits(self.context.v[reg] & ((1u128 << F::BITS) - 1)))
    }

    fn flush_input<F: Float>(&mut self, f: F) -> F {
        if self.context.fpcr & (1 << 24) != 0 && f.is_denormal() {
            self.context.fpsr |= 1 << 7;
            return F::ZERO.copy_sign(f);
        }
        f
    }

    fn fp_result<F: Float>(&mut self, dst: usize, result: StatusAnd<F>) {
        self.context.v[dst] = self.fp_bits(result);
    }

    fn fp_bits<F: Float>(&mut self, result: StatusAnd<F>) -> u128 {
        self.context.fpsr |= u64::from(result.status.bits());
        let f = result.value;
        if self.context.fpcr & (1 << 25) != 0 && f.is_nan() {
            F::NAN
        } else if self.context.fpcr & (1 << 24) != 0 && f.is_denormal() {
            self.context.fpsr |= 1 << 3;
            F::ZERO.copy_sign(f)
        } else {
            f
        }
        .to_bits()
    }

    pub fn vector_float(&mut self, w: u32) -> Result<(), Fault> {
        if w & (1 << 22) != 0 {
            self.vector_math::<Double>(w)
        } else {
            self.vector_math::<Single>(w)
        }
    }

    fn vector_math<F: Float>(&mut self, w: u32) -> Result<(), Fault> {
        let d = (w & 31) as usize;
        let regs = [
            self.context.v[d],
            self.context.v[((w >> 5) & 31) as usize],
            self.context.v[((w >> 16) & 31) as usize],
        ];
        let indexed = w & (1 << 24) != 0;
        let q = w & (1 << 30) != 0;
        let scalar = w & (1 << 28) != 0;
        if !q && (scalar || F::BITS == 64) {
            return Err(Fault::Instruction(w));
        }
        let count = if scalar {
            1
        } else if q {
            128 / F::BITS
        } else {
            64 / F::BITS
        };
        let h = ((w >> 11) & 1) as usize;
        let l = ((w >> 21) & 1) as usize;
        if indexed && F::BITS == 64 && l != 0 {
            return Err(Fault::Instruction(w));
        }
        let index = if F::BITS == 64 { h } else { h * 2 + l };
        let mask = (1u128 << F::BITS) - 1;
        let lane = |r: usize, i: usize| F::from_bits((regs[r] >> (F::BITS * i)) & mask);
        let round = rounding((self.context.fpcr >> 22) as u32);
        let mut out = 0;
        for i in 0..count {
            let a = self.flush_input(lane(1, i));
            if w & 0x003e0000 == 0x00200000
                && w & 0x00008800 == 0x00008800
                && matches!(
                    w & 0xbfbffc00,
                    0x0e218800
                        | 0x0e219800
                        | 0x0ea18800
                        | 0x0ea19800
                        | 0x2e218800
                        | 0x2e219800
                        | 0x2ea19800
                )
            {
                let mode = match w & 0xbfbffc00 {
                    0x0e218800 => Round::NearestTiesToEven,
                    0x0e219800 => Round::TowardNegative,
                    0x0ea18800 => Round::TowardPositive,
                    0x0ea19800 => Round::TowardZero,
                    0x2e218800 => Round::NearestTiesToAway,
                    _ => round,
                };
                let mut result = a.round_to_integral(mode);
                if w & 0xbfbffc00 != 0x2e219800 {
                    result.status.remove(Status::INEXACT);
                }
                out |= self.fp_bits(result) << (i * F::BITS);
                continue;
            }
            let b = self.flush_input(lane(2, if indexed { index } else { i }));
            let op = if indexed {
                match (w >> 12) & 15 {
                    1 => 0x0e20cc00,
                    5 => 0x0ea0cc00,
                    9 => 0x2e20dc00,
                    _ => return Err(Fault::Instruction(w)),
                }
            } else {
                w & 0xbfa0fc00
            };
            let calc = match op {
                0x0e20d400 => a.add_r(b, round),
                0x0ea0d400 => a.sub_r(b, round),
                0x2e20dc00 => a.mul_r(b, round),
                0x2e20fc00 => a.div_r(b, round),
                0x0e20cc00 | 0x0ea0cc00 => {
                    let acc = self.flush_input(lane(0, i));
                    (if op == 0x0ea0cc00 { -a } else { a }).mul_add_r(b, acc, round)
                }
                _ => return Err(Fault::Instruction(w)),
            };
            out |= self.fp_bits(calc) << (i * F::BITS);
        }
        self.context.v[d] = out;
        Ok(())
    }

    pub fn convert_lanes(&mut self, w: u32) -> Result<(), Fault> {
        let wide = w & (1 << 22) != 0;
        let scalar = w & (1 << 28) != 0;
        let q = w & (1 << 30) != 0;
        if !q && (scalar || wide) {
            return Err(Fault::Instruction(w));
        }
        let bits = if wide { 64 } else { 32 };
        let count = if scalar {
            1
        } else if q {
            128 / bits
        } else {
            64 / bits
        };
        let src = self.context.v[((w >> 5) & 31) as usize];
        let round = rounding((self.context.fpcr >> 22) as u32);
        let signed = w & (1 << 29) == 0;
        let mut value = 0;
        for i in 0..count {
            let input = (src >> (i * bits)) & ((1u128 << bits) - 1);
            let result = if w & (1 << 23) != 0 {
                if wide {
                    self.lane_to_integer::<Double>(input, signed)
                } else {
                    self.lane_to_integer::<Single>(input, signed)
                }
            } else if wide {
                from_integer::<Double>(input as u64, bits, signed, round).map(Float::to_bits)
            } else {
                from_integer::<Single>(input as u64, bits, signed, round).map(Float::to_bits)
            };
            self.context.fpsr |= u64::from(result.status.bits());
            value |= (result.value & ((1u128 << bits) - 1)) << (i * bits);
        }
        self.context.v[(w & 31) as usize] = value;
        Ok(())
    }

    fn lane_to_integer<F: Float>(&mut self, input: u128, signed: bool) -> StatusAnd<u128> {
        let f = self.flush_input(F::from_bits(input));
        if signed {
            f.to_i128_r(F::BITS, Round::TowardZero, &mut false)
                .map(|v| v as u128)
        } else {
            f.to_u128_r(F::BITS, Round::TowardZero, &mut false)
        }
    }

    pub fn float(&mut self, w: u32) -> Result<(), Fault> {
        match (w >> 22) & 3 {
            0 => self.scalar::<Single, Double>(w),
            1 => self.scalar::<Double, Single>(w),
            _ => Err(Fault::Instruction(w)),
        }
    }

    fn scalar<F: Float + FloatConvert<Other>, Other: Float>(
        &mut self,
        w: u32,
    ) -> Result<(), Fault> {
        let d = (w & 31) as usize;
        let n = ((w >> 5) & 31) as usize;
        let m = ((w >> 16) & 31) as usize;
        let round = rounding((self.context.fpcr >> 22) as u32);
        let bits = if w >> 31 == 0 { 32 } else { 64 };
        let value = self.reg(n as u32);
        if w & 0xff000000 == 0x1f000000 {
            let a = self.fp_input::<F>(n);
            let b = self.fp_input::<F>(m);
            let c = self.fp_input::<F>(((w >> 10) & 31) as usize);
            let neg = w & (1 << 21) != 0;
            let sub = w & (1 << 15) != 0;
            let a = if sub ^ neg { -a } else { a };
            let c = if neg { -c } else { c };
            self.fp_result(d, a.mul_add_r(b, c, round));
            return Ok(());
        }
        if matches!(w & 0x7fbe0000, 0x1e020000 | 0x1e180000) {
            let fraction = 64 - ((w >> 10) & 63);
            if fraction as usize > bits {
                return Err(Fault::Instruction(w));
            }
            let signed = w & (1 << 16) == 0;
            if w & (1 << 19) == 0 {
                let f = from_integer::<F>(value, bits, signed, round)
                    .map(|v| v.scalbn(-(fraction as i32)));
                self.fp_result(d, f);
            } else {
                let f = self.fp_input::<F>(n).scalbn(fraction as i32);
                let result = if signed {
                    f.to_i128_r(bits, Round::TowardZero, &mut false)
                        .map(|v| v as u64)
                } else {
                    f.to_u128_r(bits, Round::TowardZero, &mut false)
                        .map(|v| v as u64)
                };
                self.context.fpsr |= u64::from(result.status.bits());
                self.set(
                    d as u32,
                    if bits == 32 {
                        result.value as u32 as u64
                    } else {
                        result.value
                    },
                );
            }
            return Ok(());
        }
        if w & 0x7fbffc00 == 0x1e220000 || w & 0x7fbffc00 == 0x1e230000 {
            let signed = w & (1 << 16) == 0;
            self.fp_result(d, from_integer::<F>(value, bits, signed, round));
            return Ok(());
        }
        if w & 0x7fa6fc00 == 0x1e200000 || w & 0x7fbefc00 == 0x1e240000 {
            let a = self.fp_input::<F>(n);
            let mode = if w & (1 << 18) != 0 {
                Round::NearestTiesToAway
            } else {
                rounding(w >> 19)
            };
            let result = if w & (1 << 16) == 0 {
                a.to_i128_r(bits, mode, &mut false).map(|i| i as u64)
            } else {
                a.to_u128_r(bits, mode, &mut false).map(|i| i as u64)
            };
            self.context.fpsr |= u64::from(result.status.bits());
            self.set(
                d as u32,
                if bits == 32 {
                    result.value as u32 as u64
                } else {
                    result.value
                },
            );
            return Ok(());
        }
        let mask = (1u128 << F::BITS) - 1;
        let sign = 1u128 << (F::BITS - 1);
        if w & 0xffbf7c00 == 0x1e204000 || w & 0xffbffc00 == 0x1e214000 {
            let a = self.context.v[n] & mask;
            self.context.v[d] = match (w >> 15) & 3 {
                0 => a,
                1 => a & !sign,
                _ => a ^ sign,
            };
            return Ok(());
        }
        if w & 0xff201fe0 == 0x1e201000 {
            let imm = (w >> 13) & 255;
            self.context.v[d] = u128::from(expand_immediate(imm, F::BITS == 64));
            return Ok(());
        }
        if w & 0xff200c00 == 0x1e200c00 {
            self.context.v[d] =
                self.context.v[if self.condition((w >> 12) & 15) { n } else { m }] & mask;
            return Ok(());
        }
        let conditional_compare = w & 0xff200c00 == 0x1e200400;
        if conditional_compare && !self.condition((w >> 12) & 15) {
            self.f.pstate = (self.f.pstate & !0xf0000000) | (u64::from(w & 15) << 28);
            return Ok(());
        }
        let a = self.fp_input::<F>(n);
        if w & 0xffbc7c00 == 0x1e244000 {
            let op = (w >> 15) & 7;
            let mode = match op {
                0..=3 => rounding(op),
                4 => Round::NearestTiesToAway,
                6 | 7 => round,
                _ => return Err(Fault::Instruction(w)),
            };
            let mut result = a.round_to_integral(mode);
            if op != 6 {
                result.status.remove(Status::INEXACT);
            }
            self.fp_result(d, result);
            return Ok(());
        }
        if matches!(w & 0xfffffc00, 0x1e624000 | 0x1e22c000) {
            let result = a.convert_r(round, &mut false);
            self.fp_result::<Other>(d, result);
            return Ok(());
        }
        if conditional_compare || w & 0xff20fc07 == 0x1e202000 {
            let zero = !conditional_compare && w & 8 != 0;
            if zero && m != 0 {
                return Err(Fault::Instruction(w));
            }
            let b = if zero { F::ZERO } else { self.fp_input::<F>(m) };
            let flags = if a.is_nan() || b.is_nan() {
                if a.is_signaling() || b.is_signaling() || w & 16 != 0 {
                    self.context.fpsr |= u64::from(Status::INVALID_OP.bits());
                }
                3
            } else if a < b {
                8
            } else if a == b {
                6
            } else {
                2
            };
            self.f.pstate = (self.f.pstate & !0xf0000000) | (flags << 28);
            return Ok(());
        }
        if w & 0xff200c00 == 0x1e200800 {
            let b = self.fp_input::<F>(m);
            let result = match (w >> 12) & 15 {
                0 => a.mul_r(b, round),
                1 => a.div_r(b, round),
                2 => a.add_r(b, round),
                3 => a.sub_r(b, round),
                op @ 4..=7 => minmax(a, b, op & 1 != 0, op & 2 != 0),
                8 => a.mul_r(b, round).map(|x| -x),
                _ => return Err(Fault::Instruction(w)),
            };
            self.fp_result(d, result);
            return Ok(());
        }
        Err(Fault::Instruction(w))
    }
}
