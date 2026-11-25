#[derive(Clone, Copy)]
pub enum Integer {
    MoveWide,
    AddImmediate,
    AddCarry,
    AddExtended,
    AddShifted,
    Logical,
    Extract,
    Bitfield,
    Select,
    Compare,
    ReverseBits,
    ReverseBytes,
    LeadingZeros,
    LeadingSigns,
    VariableShift,
    Divide,
    MultiplyHigh,
    MultiplyLong,
    MultiplyAdd,
}
#[derive(Clone, Copy)]
pub enum Op {
    Branch,
    BranchRegister,
    Conditional,
    CompareBranch,
    TestBranch,
    Address,
    Syscall,
    Breakpoint,
    SystemRegister,
    CacheMaintenance,
    Float,
    Vector,
    VectorFloat(super::neon_float::Kind),
    NeonInteger(super::neon_integer::Family),
    Crypto,
    Crc,
    ClearExclusive,
    Hint,
    Transfer,
    Integer(Integer),
    Unknown,
}

pub fn classify(w: u32) -> Op {
    if w & 0x7c000000 == 0x14000000 {
        return Op::Branch;
    }
    if w & 0xfffffc1f == 0xd61f0000 || w & 0xfffffc1f == 0xd63f0000 || w & 0xfffffc1f == 0xd65f0000
    {
        return Op::BranchRegister;
    }
    if w & 0xff000010 == 0x54000000 {
        return Op::Conditional;
    }
    if w & 0x7e000000 == 0x34000000 {
        return Op::CompareBranch;
    }
    if w & 0x7e000000 == 0x36000000 {
        return Op::TestBranch;
    }
    if w & 0x1f000000 == 0x10000000 {
        return Op::Address;
    }
    if w & 0xffe0001f == 0xd4200000 {
        return Op::Breakpoint;
    }
    if w & 0xffe0001f == 0xd4000001 {
        return Op::Syscall;
    }
    if matches!(
        w & 0xffffffe0,
        0xd53bd040
            | 0xd51bd040
            | 0xd53b00e0
            | 0xd53b0020
            | 0xd53be000
            | 0xd53be020
            | 0xd53be040
            | 0xd53b4200
            | 0xd51b4200
            | 0xd53b4400
            | 0xd51b4400
            | 0xd53b4420
            | 0xd51b4420
    ) {
        return Op::SystemRegister;
    }
    if matches!(
        w & 0xffffffe0,
        0xd50b7b20 | 0xd50b7e20 | 0xd50b7a20 | 0xd50b7520
    ) {
        return Op::CacheMaintenance;
    }
    if w & 0x7fe0e000 == 0x1ac04000 {
        return Op::Crc;
    }
    if matches!(w & 0xfffefc00, 0x9e660000 | 0x1e260000 | 0x9eae0000) {
        return Op::Vector;
    }
    if w & 0x7f000000 == 0x1e000000 || w & 0xff000000 == 0x1f000000 {
        return Op::Float;
    }
    if w & 0xbfe0fc00 == 0x2e209c00
        || w & 0xffffcc00 == 0x4e284800
        || w & 0xbf20fc00 == 0x0e20e000
        || w & 0xffe08c00 == 0x5e000000
        || matches!(w & 0xfffffc00, 0x5e280800 | 0x5e281800 | 0x5e282800)
    {
        return Op::Crypto;
    }
    if let Some(op) = super::neon_float::classify(w) {
        return Op::VectorFloat(op);
    }
    if w & 0x0e000000 == 0x0e000000
        && let Some(op) = super::neon_integer::classify(w)
    {
        return Op::NeonInteger(op);
    }
    if w & 0x1e000000 == 0x0e000000 || w & 0xde000000 == 0x5e000000 {
        return Op::Vector;
    }
    if w & 0xfffff0ff == 0xd503305f {
        return Op::ClearExclusive;
    }
    if w & 0xfffff01f == 0xd503201f
        || matches!(w & 0xfffff0ff, 0xd503309f | 0xd50330bf)
        || w == 0xd5033fdf
    {
        return Op::Hint;
    }
    if (w >> 25) & 5 == 4 {
        return Op::Transfer;
    }
    if w & 0x1f800000 == 0x12800000 {
        return Op::Integer(Integer::MoveWide);
    }
    if w & 0x1fe0fc00 == 0x1a000000 {
        return Op::Integer(Integer::AddCarry);
    }
    if w & 0x1f800000 == 0x11000000 {
        return Op::Integer(Integer::AddImmediate);
    }
    if w & 0x1fe00000 == 0x0b200000 {
        return Op::Integer(Integer::AddExtended);
    }
    if w & 0x1f200000 == 0x0b000000 {
        return Op::Integer(Integer::AddShifted);
    }
    if w & 0x1f800000 == 0x12000000 || w & 0x1f000000 == 0x0a000000 {
        return Op::Integer(Integer::Logical);
    }
    if w & 0x7fa00000 == 0x13800000 {
        return Op::Integer(Integer::Extract);
    }
    if w & 0x1f800000 == 0x13000000 {
        return Op::Integer(Integer::Bitfield);
    }
    if w & 0x3fe00800 == 0x1a800000 {
        return Op::Integer(Integer::Select);
    }
    if w & 0x3fe00410 == 0x3a400000 {
        return Op::Integer(Integer::Compare);
    }
    if w & 0x7ffffc00 == 0x5ac00000 {
        return Op::Integer(Integer::ReverseBits);
    }
    if matches!(w & 0x7ffffc00, 0x5ac00400 | 0x5ac00800) || w & 0xfffffc00 == 0xdac00c00 {
        return Op::Integer(Integer::ReverseBytes);
    }
    if w & 0x7ffffc00 == 0x5ac01400 {
        return Op::Integer(Integer::LeadingSigns);
    }
    if w & 0x7ffffc00 == 0x5ac01000 {
        return Op::Integer(Integer::LeadingZeros);
    }
    if w & 0x7fe0f000 == 0x1ac02000 {
        return Op::Integer(Integer::VariableShift);
    }
    if w & 0x7fe0f800 == 0x1ac00800 {
        return Op::Integer(Integer::Divide);
    }
    if w & 0xff60fc00 == 0x9b407c00 {
        return Op::Integer(Integer::MultiplyHigh);
    }
    if w & 0xff600000 == 0x9b200000 {
        return Op::Integer(Integer::MultiplyLong);
    }
    if w & 0x7fe00000 == 0x1b000000 {
        return Op::Integer(Integer::MultiplyAdd);
    }
    Op::Unknown
}
