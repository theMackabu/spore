use rustc_apfloat::{Float, Round, Status, StatusAnd};

#[derive(Clone, Copy)]
pub enum Estimate {
    Reciprocal,
    Root,
    Exponent,
}

pub fn fixed(index: u32, root: bool) -> u32 {
    if root {
        let midpoint = if index < 256 {
            2 * index + 1
        } else {
            2 * ((index & !1) + 1)
        };
        (((1u32 << 28) - 1) / midpoint).isqrt().div_ceil(2)
    } else {
        ((1u32 << 19) / (2 * index + 1)).div_ceil(2)
    }
}
pub fn floating<F: Float>(a: F, kind: Estimate, round: Round) -> StatusAnd<F> {
    let frac = F::PRECISION - 1;
    let max_exp = (1u128 << (F::BITS - F::PRECISION)) - 1;
    let sign = a.to_bits() & (1u128 << (F::BITS - 1));
    let exp = (a.to_bits() >> frac) & max_exp;
    if a.is_nan() {
        let status = if a.is_signaling() {
            Status::INVALID_OP
        } else {
            Status::OK
        };
        return status.and(F::from_bits(a.to_bits() | (1 << (frac - 1))));
    }
    if matches!(kind, Estimate::Exponent) {
        let out_exp = if exp == 0 { max_exp - 1 } else { max_exp - exp };
        return Status::OK.and(F::from_bits(sign | (out_exp << frac)));
    }
    if a.is_zero() {
        return Status::DIV_BY_ZERO.and(F::INFINITY.copy_sign(a));
    }
    let root = matches!(kind, Estimate::Root);
    if root && a.is_negative() {
        return Status::INVALID_OP.and(F::NAN);
    }
    if a.is_infinite() {
        return Status::OK.and(F::ZERO.copy_sign(a));
    }
    let power = a.ilogb();
    let sig = (a.to_bits() & ((1 << frac) - 1)) | (u128::from(exp != 0) << frac);
    let precision = if root && power % 2 == 0 { 7 } else { 8 };
    let index = ((sig << sig.leading_zeros()) >> (127 - precision)) as u32;
    let result_exp = if root {
        -power.div_euclid(2) - 1
    } else {
        -power - 1
    };
    if result_exp > F::MAX_EXP {
        let to_inf = matches!(round, Round::NearestTiesToEven | Round::NearestTiesToAway)
            || round == Round::TowardPositive && !a.is_negative()
            || round == Round::TowardNegative && a.is_negative();
        return (Status::OVERFLOW | Status::INEXACT)
            .and((if to_inf { F::INFINITY } else { F::largest() }).copy_sign(a));
    }
    Status::OK.and(
        F::from_u128(u128::from(fixed(index, root)))
            .value
            .scalbn(result_exp - 8)
            .copy_sign(a),
    )
}
