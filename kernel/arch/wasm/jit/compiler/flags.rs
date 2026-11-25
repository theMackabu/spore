use super::{Block, FLAGS, I, LEFT, RETIRED, RIGHT, VALUE};
impl Block {
    pub(super) fn flags(&mut self, bits: u32, arithmetic: bool, sub: bool) {
        self.flags_written = true;
        self.body.extend([
            I::LocalGet(VALUE),
            I::I64Const(i64::from(bits - 1)),
            I::I64ShrU,
            I::I64Const(3),
            I::I64Shl,
            I::LocalGet(VALUE),
            I::I64Eqz,
            I::I64ExtendI32U,
            I::I64Const(2),
            I::I64Shl,
            I::I64Or,
        ]);
        if arithmetic {
            if sub {
                self.body
                    .extend([I::LocalGet(LEFT), I::LocalGet(RIGHT), I::I64GeU]);
            } else {
                self.body
                    .extend([I::LocalGet(VALUE), I::LocalGet(LEFT), I::I64LtU]);
            }
            self.body.extend([
                I::I64ExtendI32U,
                I::I64Const(1),
                I::I64Shl,
                I::I64Or,
                I::LocalGet(LEFT),
                I::LocalGet(RIGHT),
                I::I64Xor,
            ]);
            if !sub {
                self.body.extend([I::I64Const(-1), I::I64Xor]);
            }
            self.body.extend([
                I::LocalGet(LEFT),
                I::LocalGet(VALUE),
                I::I64Xor,
                I::I64And,
                I::I64Const(i64::from(bits - 1)),
                I::I64ShrU,
                I::I64Const(1),
                I::I64And,
                I::I64Or,
            ]);
        }
        self.body.push(I::LocalSet(FLAGS));
    }

    fn flag(&mut self, bit: u32) {
        self.body.extend([
            I::LocalGet(FLAGS),
            I::I32WrapI64,
            I::I32Const(bit as i32),
            I::I32ShrU,
            I::I32Const(1),
            I::I32And,
        ]);
    }
    pub(super) fn pack_flags(&mut self) {
        if let Some((bits, arithmetic, sub)) = self.pending {
            self.flags(bits, arithmetic, sub);
        }
    }

    pub(super) fn side_exit(&mut self, retired: usize) {
        self.flags_read |= !self.flags_written;
        self.pack_flags();
        self.body.extend([
            I::LocalGet(RETIRED),
            I::I32Const(retired as i32),
            I::I32Add,
            I::LocalSet(RETIRED),
        ]);
        self.side_exits.push((self.body.len(), self.dirty));
        self.body.extend([I::Br(2), I::End]);
    }

    pub(super) fn condition(&mut self, cond: u32) {
        if let Some((bits, arithmetic, sub)) = self.pending {
            let family = cond >> 1;
            if family == 0 {
                self.body.extend([I::LocalGet(VALUE), I::I64Eqz]);
            } else if arithmetic && sub && matches!(family, 1 | 4 | 5 | 6) {
                for local in [LEFT, RIGHT] {
                    self.body.push(I::LocalGet(local));
                    if family >= 5 && bits == 32 {
                        self.body.push(I::I64Extend32S);
                    }
                }
                self.body.push(match family {
                    1 => I::I64GeU,
                    4 => I::I64GtU,
                    5 => I::I64GeS,
                    _ => I::I64GtS,
                });
            } else {
                self.pack_flags();
                self.pending = None;
                self.condition(cond);
                return;
            }
            if cond & 1 != 0 {
                self.body.push(I::I32Eqz);
            }
            return;
        }
        self.flags_read = true;
        match cond >> 1 {
            0 => self.flag(2),
            1 => self.flag(1),
            2 => self.flag(3),
            3 => self.flag(0),
            4 => {
                self.flag(1);
                self.flag(2);
                self.body.extend([I::I32Eqz, I::I32And]);
            }
            5 | 6 => {
                self.flag(3);
                self.flag(0);
                self.body.push(I::I32Eq);
                if cond >> 1 == 6 {
                    self.flag(2);
                    self.body.extend([I::I32Eqz, I::I32And]);
                }
            }
            _ => {
                self.body.push(I::I32Const(1));
                return;
            }
        }
        if cond & 1 != 0 {
            self.body.push(I::I32Eqz);
        }
    }
}
