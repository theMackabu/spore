use super::cpu::{Cpu, Fault};
use rustc_apfloat::{
    Float, FloatConvert, Round, Status, StatusAnd,
    ieee::{Double, Half, Single},
};

pub fn expand_immediate(imm: u32, wide: bool) -> u64 {
    let (fraction, exponent) = if wide { (52, 11) } else { (23, 8) };
    let b = (imm >> 6) & 1;
    let exp = ((1 - b) << (exponent - 1)) | (b * ((1 << (exponent - 1)) - 4)) | ((imm >> 4) & 3);
    (u64::from(imm >> 7) << (fraction + exponent))
        | (u64::from(exp) << fraction)
        | (u64::from(imm & 15) << (fraction - 4))
}

pub(super) fn rounding(bits: u32) -> Round {
    match bits & 3 {
        0 => Round::NearestTiesToEven,
        1 => Round::TowardPositive,
        2 => Round::TowardNegative,
        _ => Round::TowardZero,
    }
}

pub(super) fn from_integer<F: Float>(
    value: u64,
    bits: usize,
    signed: bool,
    round: Round,
) -> StatusAnd<F> {
    match (signed, bits) {
        (true, 32) => F::from_i128_r(value as i32 as i128, round),
        (true, _) => F::from_i128_r(value as i64 as i128, round),
        (false, 32) => F::from_u128_r(value as u32 as u128, round),
        (false, _) => F::from_u128_r(value as u128, round),
    }
}

pub(super) fn square_root<F: Float>(a: F, round: Round) -> StatusAnd<F> {
    if a.is_nan() {
        let status = if a.is_signaling() {
            Status::INVALID_OP
        } else {
            Status::OK
        };
        return status.and(F::from_bits(a.to_bits() | (1 << (F::PRECISION - 2))));
    }
    if a.is_zero() || a.is_pos_infinity() {
        return Status::OK.and(a);
    }
    if a.is_negative() {
        return Status::INVALID_OP.and(F::NAN);
    }
    let bits = a.to_bits();
    let mut result = if F::BITS == 32 {
        u128::from(f32::from_bits(bits as u32).sqrt().to_bits())
    } else {
        u128::from(f64::from_bits(bits as u64).sqrt().to_bits())
    };
    let fraction = F::PRECISION - 1;
    let bias = (1i32 << (F::BITS - F::PRECISION - 1)) - 1;
    let split = |value: u128| {
        let exponent = (value >> fraction) as i32;
        let sig = (value & ((1 << fraction) - 1)) | (u128::from(exponent != 0) << fraction);
        (sig, exponent.max(1) - bias - fraction as i32)
    };
    let (root, er) = split(result);
    let (arg, ea) = split(bits);
    let norm = |significand: u128, exp: i32| {
        let zeros = significand.leading_zeros();
        (exp - zeros as i32, significand << zeros)
    };
    let direction = norm(root * root, 2 * er).cmp(&norm(arg, ea));
    if direction.is_eq() {
        return Status::OK.and(F::from_bits(result));
    }
    if direction.is_lt() && round == Round::TowardPositive {
        result += 1;
    }
    if direction.is_gt() && matches!(round, Round::TowardZero | Round::TowardNegative) {
        result -= 1;
    }
    Status::INEXACT.and(F::from_bits(result))
}

pub(super) fn minmax<F: Float>(a: F, b: F, minimum: bool, numeric: bool) -> StatusAnd<F> {
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

    pub(super) fn flush_input<F: Float>(&mut self, f: F) -> F {
        if self.context.fpcr & (1 << 24) != 0 && f.is_denormal() {
            self.context.fpsr |= 1 << 7;
            return F::ZERO.copy_sign(f);
        }
        f
    }

    fn fp_result<F: Float>(&mut self, dst: usize, result: StatusAnd<F>) {
        self.context.v[dst] = self.fp_bits(result);
    }

    pub(super) fn fp_bits<F: Float>(&mut self, result: StatusAnd<F>) -> u128 {
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

    pub fn float(&mut self, w: u32) -> Result<(), Fault> {
        let n = ((w >> 5) & 31) as usize;
        let d = (w & 31) as usize;
        let round = rounding((self.context.fpcr >> 22) as u32);
        macro_rules! convert {
            ($src:ty, $dst:ty) => {{
                let a = self.fp_input::<$src>(n);
                let converted: StatusAnd<$dst> = a.convert_r(round, &mut false);
                self.fp_result(d, converted);
                return Ok(());
            }};
        }
        match w & 0xfffffc00 {
            0x1e23c000 => convert!(Single, Half),
            0x1e63c000 => convert!(Double, Half),
            0x1ee24000 => convert!(Half, Single),
            0x1ee2c000 => convert!(Half, Double),
            0x1e624000 => convert!(Double, Single),
            0x1e22c000 => convert!(Single, Double),
            _ => {}
        }
        match (w >> 22) & 3 {
            0 => self.scalar::<Single>(w),
            1 => self.scalar::<Double>(w),
            _ => Err(Fault::Instruction(w)),
        }
    }

    fn scalar<F: Float>(&mut self, w: u32) -> Result<(), Fault> {
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
        if w & 0xffbffc00 == 0x1e21c000 {
            self.fp_result(d, square_root(a, round));
            return Ok(());
        }
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
