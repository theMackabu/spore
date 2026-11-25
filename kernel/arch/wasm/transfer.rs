use super::cpu::{Cpu, Fault, sx};

impl Cpu {
    fn transfer_one(
        &mut self,
        reg: u32,
        addr: u64,
        size: usize,
        load: bool,
        vector: bool,
        sign_bits: u32,
    ) -> Result<(), Fault> {
        if load {
            let v = self.memory.load(addr, size)?;
            if vector {
                self.context.v[reg as usize] = v;
            } else {
                let mut v = v as u64;
                if sign_bits != 0 {
                    v = sx(v, size as u32 * 8);
                    if sign_bits == 32 {
                        v = v as u32 as u64;
                    }
                }
                self.set(reg, v);
            }
        } else {
            let v = if vector {
                self.context.v[reg as usize]
            } else {
                self.reg(reg) as u128
            };
            self.reservation = None;
            self.memory.store(addr, size, v)?;
        }
        Ok(())
    }

    pub fn transfer(&mut self, w: u32) -> Result<(), Fault> {
        let (t, n) = (w & 31, (w >> 5) & 31);
        let base = self.base(n);
        if n == 31 && base & 15 != 0 {
            return Err(Fault::Alignment(base));
        }
        let vector = w & (1 << 26) != 0;
        let scale = w >> 30;
        if w & 0x3ffffc00 == 0x08dffc00 || w & 0x3fff7c00 == 0x085f7c00 {
            let size = 1usize << scale;
            if base & (size as u64 - 1) != 0 {
                return Err(Fault::Alignment(base));
            }
            let value = self.memory.load(base, size)?;
            self.set(t, value as u64);
            if w & (1 << 23) == 0 {
                self.reservation = Some((base, size));
            }
            return Ok(());
        }
        if w & 0x3fe07c00 == 0x08007c00 || w & 0x3ffffc00 == 0x089ffc00 {
            let size = 1usize << scale;
            if base & (size as u64 - 1) != 0 {
                return Err(Fault::Alignment(base));
            }
            let exclusive = w & (1 << 23) == 0;
            let reserved = self.reservation.take() == Some((base, size));
            if !exclusive || reserved {
                self.memory.store(base, size, self.reg(t) as u128)?;
            }
            if exclusive {
                self.set((w >> 16) & 31, (!reserved) as u64);
            }
            return Ok(());
        }
        if w & 0x3a000000 == 0x28000000 {
            let mode = (w >> 23) & 3;
            let load = w & (1 << 22) != 0;
            let size = if vector && scale < 3 {
                4 << scale
            } else if !vector && scale == 2 {
                8
            } else if !vector && (scale == 0 || (scale == 1 && load && mode != 0)) {
                4
            } else {
                return Err(Fault::Instruction(w));
            };
            let off = sx(((w >> 15) & 127) as u64, 7).wrapping_mul(size as u64);
            let updated = base.wrapping_add(off);
            let addr = if mode == 1 { base } else { updated };
            let sign = if !vector && scale == 1 { 64 } else { 0 };
            self.transfer_one(t, addr, size, load, vector, sign)?;
            self.transfer_one(
                (w >> 10) & 31,
                addr.wrapping_add(size as u64),
                size,
                load,
                vector,
                sign,
            )?;
            if mode & 1 != 0 {
                self.set_base(n, updated);
            }
            return Ok(());
        }
        if w & 0x3a000000 != 0x38000000 {
            return Err(Fault::Instruction(w));
        }
        let op = (w >> 22) & 3;
        let wide = vector && op >= 2;
        if wide && scale != 0 || !vector && op >= 2 && (scale == 3 || (op == 3 && scale == 2)) {
            return Err(Fault::Instruction(w));
        }
        let log_size = if wide { 4 } else { scale };
        let size = 1usize << log_size;
        let mut writeback = false;
        let mut post = false;
        let off = if w & (1 << 24) != 0 {
            (((w >> 10) & 4095) as u64) << log_size
        } else if w & (1 << 21) != 0 {
            if (w >> 10) & 3 != 2 {
                return Err(Fault::Instruction(w));
            }
            let m = self.reg((w >> 16) & 31);
            let ext = match (w >> 13) & 7 {
                2 => m as u32 as u64,
                3 | 7 => m,
                6 => sx(m, 32),
                _ => return Err(Fault::Instruction(w)),
            };
            ext << (if w & (1 << 12) != 0 { log_size } else { 0 })
        } else {
            let mode = (w >> 10) & 3;
            if mode == 2 {
                return Err(Fault::Instruction(w));
            }
            writeback = mode != 0;
            post = mode == 1;
            sx(((w >> 12) & 511) as u64, 9)
        };
        let updated = base.wrapping_add(off);
        let sign = if !vector && op >= 2 {
            if op == 2 { 64 } else { 32 }
        } else {
            0
        };
        self.transfer_one(
            t,
            if post { base } else { updated },
            size,
            if vector { op & 1 != 0 } else { op != 0 },
            vector,
            sign,
        )?;
        if writeback {
            self.set_base(n, updated);
        }
        Ok(())
    }
}
