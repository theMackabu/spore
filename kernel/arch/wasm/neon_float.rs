use super::cpu::{Cpu, Fault};
use super::estimate::{self, Estimate};
use super::float::{from_integer, minmax, rounding, square_root};
use rustc_apfloat::{
    Float, FloatConvert, Round, Status, StatusAnd,
    ieee::{Double, Half, Single},
};

#[derive(Clone, Copy)]
pub enum Kind {
    Add,
    Sub,
    Mul,
    MulX,
    Div,
    AbsoluteDifference,
    Fma(bool),
    MinMax { min: bool, numeric: bool },
    PairAdd,
    PairExtrema,
    ReduceAdd,
    ReduceExtrema,
    Compare,
    CompareZero,
    Round,
    Sqrt,
    IntToFloat,
    FloatToInt,
    Widen,
    Narrow,
    ReciprocalStep,
    RootStep,
    Estimate(Estimate),
    UnsignedEstimate(bool),
}

pub fn classify(w: u32) -> Option<Kind> {
    use Kind::*;
    let three = w & 0xafa0fc00;
    let min = w & (1 << 23) != 0;
    let numeric = w & 0x3000 == 0;
    Some(match three {
        0x0e20d400 => Add,
        0x0ea0d400 => Sub,
        0x2e20dc00 => Mul,
        0x0e20dc00 => MulX,
        0x2e20fc00 => Div,
        0x2ea0d400 => AbsoluteDifference,
        0x0e20cc00 => Fma(false),
        0x0ea0cc00 => Fma(true),
        0x0e20f400 | 0x0ea0f400 | 0x0e20c400 | 0x0ea0c400 => MinMax { min, numeric },
        0x2e20d400 => PairAdd,
        0x2e20f400 | 0x2ea0f400 | 0x2e20c400 | 0x2ea0c400 => PairExtrema,
        0x0e20e400 | 0x2e20e400 | 0x2ea0e400 | 0x2e20ec00 | 0x2ea0ec00 => Compare,
        0x0e20fc00 => ReciprocalStep,
        0x0ea0fc00 => RootStep,
        _ => match w & 0xafbffc00 {
            0x0e218800 | 0x0e219800 | 0x0ea18800 | 0x0ea19800 | 0x2e218800 | 0x2e219800
            | 0x2ea19800 => Round,
            0x2ea1f800 => Sqrt,
            0x0ea1d800 => Estimate(estimate::Estimate::Reciprocal),
            0x2ea1d800 => Estimate(estimate::Estimate::Root),
            0x0ea1f800 if w & (1 << 28) != 0 => Estimate(estimate::Estimate::Exponent),
            0x0ea1c800 => UnsignedEstimate(false),
            0x2ea1c800 => UnsignedEstimate(true),
            0x0ea0d800 | 0x0ea0c800 | 0x2ea0c800 | 0x2ea0d800 | 0x0ea0e800 => CompareZero,
            0x2e30d800 => ReduceAdd,
            0x2e30f800 | 0x2eb0f800 | 0x2e30c800 | 0x2eb0c800 => ReduceExtrema,
            0x0e217800 => Widen,
            0x0e216800 | 0x2e216800 => Narrow,
            _ => match w & 0x8fbffc00 {
                0x0e21d800 => IntToFloat,
                0x0e21a800 | 0x0ea1a800 | 0x0e21b800 | 0x0ea1b800 | 0x0e21c800 => FloatToInt,
                _ => match w & 0xaf80f400 {
                    0x0f801000 => Fma(false),
                    0x0f805000 => Fma(true),
                    0x0f809000 => Mul,
                    0x2f809000 => MulX,
                    _ if w & 0x00780000 != 0 && w & 0x8f80fc00 == 0x0f00e400 => IntToFloat,
                    _ if w & 0x00780000 != 0 && w & 0x8f80fc00 == 0x0f00fc00 => FloatToInt,
                    _ => return None,
                },
            },
        },
    })
}
impl Cpu {
    fn resize_float<A: Float + FloatConvert<B>, B: Float>(
        &mut self,
        w: u32,
        narrow: bool,
        odd: bool,
    ) -> u128 {
        let n = self.context.v[((w >> 5) & 31) as usize];
        let upper = w & (1 << 30) != 0 && w & (1 << 28) == 0;
        let start = if !narrow && upper { 64 / A::BITS } else { 0 };
        let count = if w & (1 << 28) != 0 {
            1
        } else {
            128 / A::BITS.max(B::BITS)
        };
        let round = if odd {
            Round::TowardZero
        } else {
            rounding((self.context.fpcr >> 22) as u32)
        };
        let mut out = 0;
        for i in 0..count {
            let a = self.flush_input(A::from_bits(
                (n >> ((i + start) * A::BITS)) & ((1 << A::BITS) - 1),
            ));
            let res: StatusAnd<B> = a.convert_r(round, &mut false);
            let jam = odd && res.status.contains(Status::INEXACT);
            out |= (self.fp_bits(res) | u128::from(jam)) << (B::BITS * i);
        }
        if narrow && upper {
            (self.context.v[(w & 31) as usize] & u128::from(u64::MAX)) | (out << 64)
        } else {
            out
        }
    }

    pub fn neon_float(&mut self, w: u32, kind: Kind) -> Result<(), Fault> {
        let fixed = matches!(kind, Kind::IntToFloat | Kind::FloatToInt) && w & (1 << 24) != 0;
        let wide = w & (1 << 22) != 0;
        let d = (w & 31) as usize;
        if let Kind::UnsignedEstimate(root) = kind {
            let src = self.context.v[((w >> 5) & 31) as usize];
            let count = if w & (1 << 30) == 0 { 2 } else { 4 };
            let mut out = 0;
            for i in 0..count {
                let idx = ((src >> (32 * i)) as u32) >> 23;
                let result = if idx < (if root { 128 } else { 256 }) {
                    u32::MAX
                } else {
                    estimate::fixed(idx, root) << 23
                };
                out |= u128::from(result) << (i * 32);
            }
            self.context.v[d] = out;
            return Ok(());
        }
        if matches!(kind, Kind::Widen | Kind::Narrow) {
            self.context.v[d] = match (kind, wide) {
                (Kind::Widen, true) => self.resize_float::<Single, Double>(w, false, false),
                (Kind::Widen, false) => self.resize_float::<Half, Single>(w, false, false),
                (Kind::Narrow, true) => {
                    self.resize_float::<Double, Single>(w, true, w & (1 << 29) != 0)
                }
                (Kind::Narrow, false) if w & (1 << 29) == 0 => {
                    self.resize_float::<Single, Half>(w, true, false)
                }
                _ => return Err(Fault::Instruction(w)),
            };
            return Ok(());
        }
        if wide {
            self.neon_math::<Double>(w, kind, fixed)
        } else {
            self.neon_math::<Single>(w, kind, fixed)
        }
    }

    fn neon_math<F: Float>(&mut self, w: u32, kind: Kind, fixed: bool) -> Result<(), Fault> {
        use Kind::*;
        let d = (w & 31) as usize;
        let n = ((w >> 5) & 31) as usize;
        let m = ((w >> 16) & 31) as usize;
        let regs = [self.context.v[d], self.context.v[n], self.context.v[m]];
        let scalar = w & (1 << 28) != 0;
        let q = w & (1 << 30) != 0;
        let indexed = w & (1 << 24) != 0 && !fixed;
        let bits = F::BITS;
        let signed = w & (1 << 29) == 0;
        if !q && (scalar || bits == 64) {
            return Err(Fault::Instruction(w));
        }
        let count = if scalar {
            1
        } else if q {
            128 / bits
        } else {
            64 / bits
        };
        let upper = ((w >> 11) & 1) as usize;
        let lower = ((w >> 21) & 1) as usize;
        if indexed && bits == 64 && lower != 0 {
            return Err(Fault::Instruction(w));
        }
        let index = if bits == 64 { upper } else { 2 * upper + lower };
        let mask = (1u128 << bits) - 1;
        let lane = |r: usize, i: usize| (regs[r] >> (i * bits)) & mask;
        let round = rounding((self.context.fpcr >> 22) as u32);
        let min = w & (1 << 23) != 0;
        let numeric = w & 0x3000 == 0;
        if matches!(kind, ReduceAdd | ReduceExtrema) {
            let elements = if scalar { 2 } else { 4 };
            if !scalar && (bits != 32 || !q || matches!(kind, ReduceAdd)) {
                return Err(Fault::Instruction(w));
            }
            let mut acc = self.flush_input(F::from_bits(lane(1, 0)));
            for i in 1..elements {
                let b = self.flush_input(F::from_bits(lane(1, i)));
                let res = if matches!(kind, ReduceAdd) {
                    acc.add_r(b, round)
                } else {
                    minmax(acc, b, min, numeric)
                };
                acc = F::from_bits(self.fp_bits(res));
            }
            self.context.v[d] = acc.to_bits();
            return Ok(());
        }
        let fraction = if fixed {
            let imm = ((w >> 16) & 127) as usize;
            if imm < bits || imm >= bits * 2 {
                return Err(Fault::Instruction(w));
            }
            (2 * bits - imm) as i32
        } else {
            0
        };
        let mut out = 0;
        for i in 0..count {
            if matches!(kind, IntToFloat) {
                let f = from_integer::<F>(lane(1, i) as u64, bits, signed, round)
                    .map(|x| x.scalbn(-fraction));
                out |= self.fp_bits(f) << (i * bits);
                continue;
            }
            let pair = matches!(kind, PairAdd | PairExtrema);
            let (r, j) = if pair {
                (if i < count / 2 { 1 } else { 2 }, (i % (count / 2)) * 2)
            } else {
                (1, i)
            };
            let mut a = self.flush_input(F::from_bits(lane(r, j)));
            if matches!(kind, FloatToInt) {
                let mode = if fixed {
                    rustc_apfloat::Round::TowardZero
                } else {
                    match w & 0x0080f000 {
                        0xa000 => rustc_apfloat::Round::NearestTiesToEven,
                        0x80a000 => rustc_apfloat::Round::TowardPositive,
                        0xb000 => rustc_apfloat::Round::TowardNegative,
                        0xc000 => rustc_apfloat::Round::NearestTiesToAway,
                        _ => rustc_apfloat::Round::TowardZero,
                    }
                };
                a = a.scalbn(fraction);
                let res = if signed {
                    a.to_i128_r(bits, mode, &mut false).map(|x| x as u128)
                } else {
                    a.to_u128_r(bits, mode, &mut false)
                };
                self.context.fpsr |= u64::from(res.status.bits());
                out |= (res.value & mask) << (i * bits);
                continue;
            }
            if matches!(kind, Round) {
                let op = w & 0xafbffc00;
                let mode = match op {
                    0x0e218800 => rustc_apfloat::Round::NearestTiesToEven,
                    0x0e219800 => rustc_apfloat::Round::TowardNegative,
                    0x0ea18800 => rustc_apfloat::Round::TowardPositive,
                    0x0ea19800 => rustc_apfloat::Round::TowardZero,
                    0x2e218800 => rustc_apfloat::Round::NearestTiesToAway,
                    _ => round,
                };
                let mut res = a.round_to_integral(mode);
                if op != 0x2e219800 {
                    res.status.remove(Status::INEXACT);
                }
                out |= self.fp_bits(res) << (i * bits);
                continue;
            }
            if let Estimate(op) = kind {
                out |= self.fp_bits(estimate::floating(a, op, round)) << (i * bits);
                continue;
            }
            if matches!(kind, Sqrt) {
                out |= self.fp_bits(square_root(a, round)) << (i * bits);
                continue;
            }
            let mut b = if matches!(kind, CompareZero) {
                F::ZERO
            } else if pair {
                self.flush_input(F::from_bits(lane(r, j + 1)))
            } else {
                self.flush_input(F::from_bits(lane(2, if indexed { index } else { i })))
            };
            if matches!(kind, Compare | CompareZero) {
                let zero = matches!(kind, CompareZero);
                let eq = if zero {
                    w & 0x20003000 == 0x1000
                } else {
                    w & 0x20000000 == 0
                };
                if !zero && w & 0x800 != 0 {
                    a = a.abs();
                    b = b.abs();
                }
                if a.is_signaling() || b.is_signaling() || !eq && (a.is_nan() || b.is_nan()) {
                    self.context.fpsr |= u64::from(Status::INVALID_OP.bits());
                }
                let yes = if eq {
                    a == b
                } else if zero {
                    match w & 0x20003000 {
                        0 => a > b,
                        0x20000000 => a >= b,
                        0x20001000 => a <= b,
                        _ => a < b,
                    }
                } else if min {
                    a > b
                } else {
                    a >= b
                };
                if yes {
                    out |= mask << (i * bits);
                }
                continue;
            }
            let res = match kind {
                Add | PairAdd => a.add_r(b, round),
                Sub => a.sub_r(b, round),
                Div => a.div_r(b, round),
                AbsoluteDifference => a.sub_r(b, round).map(Float::abs),
                MulX if a.is_zero() && b.is_infinite() || b.is_zero() && a.is_infinite() => {
                    F::from_i128(if a.is_negative() != b.is_negative() {
                        -2
                    } else {
                        2
                    })
                }
                Mul | MulX => a.mul_r(b, round),
                Fma(neg) => {
                    let c = self.flush_input(F::from_bits(lane(0, i)));
                    (if neg { -a } else { a }).mul_add_r(b, c, round)
                }
                MinMax { min, numeric } => minmax(a, b, min, numeric),
                PairExtrema => minmax(a, b, min, numeric),
                ReciprocalStep | RootStep => {
                    let root = matches!(kind, RootStep);
                    let c = F::from_u128(if root { 3 } else { 2 }).value;
                    let v = if a.is_zero() && b.is_infinite() || b.is_zero() && a.is_infinite() {
                        Status::OK.and(c)
                    } else {
                        (-a).mul_add_r(b, c, round)
                    };
                    v.map(|x| if root { x.scalbn(-1) } else { x })
                }
                _ => return Err(Fault::Instruction(w)),
            };
            out |= self.fp_bits(res) << (i * bits);
        }
        self.context.v[d] = out;
        Ok(())
    }
}
